/*
 * SPI bridge driver for the Greybus "generic" SPI module.
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include "greybus.h"

struct gb_spi {
	struct gb_connection	*connection;
	u16			mode;
	u16			flags;
	u32			bits_per_word_mask;
	u8			num_chipselect;
	u32			min_speed_hz;
	u32			max_speed_hz;
};

static struct spi_master *get_master_from_spi(struct gb_spi *spi)
{
	return spi->connection->private;
}

static int tx_header_fit_operation(u32 tx_size, u32 count, size_t data_max)
{
	size_t headers_size;

	data_max -= sizeof(struct gb_spi_transfer_request);
	headers_size = (count + 1) * sizeof(struct gb_spi_transfer);

	return tx_size + headers_size > data_max ? 0 : 1;
}

static size_t calc_rx_xfer_size(u32 rx_size, u32 *tx_xfer_size, u32 len,
				size_t data_max)
{
	size_t rx_xfer_size;

	data_max -= sizeof(struct gb_spi_transfer_response);

	if (rx_size + len > data_max)
		rx_xfer_size = data_max - rx_size;
	else
		rx_xfer_size = len;

	/* if this is a write_read, for symmetry read the same as write */
	if (*tx_xfer_size && rx_xfer_size > *tx_xfer_size)
		rx_xfer_size = *tx_xfer_size;
	if (*tx_xfer_size && rx_xfer_size < *tx_xfer_size)
		*tx_xfer_size = rx_xfer_size;

	return rx_xfer_size;
}

static size_t calc_tx_xfer_size(u32 tx_size, u32 count, size_t len,
				size_t data_max)
{
	size_t headers_size;

	data_max -= sizeof(struct gb_spi_transfer_request);
	headers_size = (count + 1) * sizeof(struct gb_spi_transfer);

	if (tx_size + headers_size + len > data_max)
		return data_max - (tx_size + sizeof(struct gb_spi_transfer));

	return len;
}

/* Routines to transfer data */
static struct gb_operation *
gb_spi_operation_create(struct gb_connection *connection,
			struct spi_message *msg, u32 *total_len)
{
	struct gb_spi_transfer_request *request;
	struct spi_device *dev = msg->spi;
	struct spi_transfer *xfer;
	struct gb_spi_transfer *gb_xfer;
	struct gb_operation *operation;
	struct spi_transfer *last_xfer = NULL;
	u32 tx_size = 0, rx_size = 0, count = 0, xfer_len = 0, request_size;
	u32 tx_xfer_size = 0, rx_xfer_size = 0, last_xfer_size = 0;
	size_t data_max;
	void *tx_data;

	data_max = gb_operation_get_payload_size_max(connection);

	/* Find number of transfers queued and tx/rx length in the message */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (!xfer->tx_buf && !xfer->rx_buf) {
			dev_err(&connection->bundle->dev,
				"bufferless transfer, length %u\n", xfer->len);
			return NULL;
		}
		last_xfer = xfer;

		tx_xfer_size = 0;
		rx_xfer_size = 0;

		if (xfer->tx_buf) {
			if (!tx_header_fit_operation(tx_size, count, data_max))
				break;
			tx_xfer_size = calc_tx_xfer_size(tx_size, count,
							 xfer->len, data_max);
			last_xfer_size = tx_xfer_size;
		}

		if (xfer->rx_buf) {
			rx_xfer_size = calc_rx_xfer_size(rx_size, &tx_xfer_size,
							 xfer->len, data_max);
			last_xfer_size = rx_xfer_size;
		}

		tx_size += tx_xfer_size;
		rx_size += rx_xfer_size;

		*total_len += last_xfer_size;
		count++;

		if (xfer->len != last_xfer_size)
			break;
	}

	/*
	 * In addition to space for all message descriptors we need
	 * to have enough to hold all tx data.
	 */
	request_size = sizeof(*request);
	request_size += count * sizeof(*gb_xfer);
	request_size += tx_size;

	/* Response consists only of incoming data */
	operation = gb_operation_create(connection, GB_SPI_TYPE_TRANSFER,
					request_size, rx_size, GFP_KERNEL);
	if (!operation)
		return NULL;

	request = operation->request->payload;
	request->count = cpu_to_le16(count);
	request->mode = dev->mode;
	request->chip_select = dev->chip_select;

	gb_xfer = &request->transfers[0];
	tx_data = gb_xfer + count;	/* place tx data after last gb_xfer */

	/* Fill in the transfers array */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (last_xfer && xfer == last_xfer)
			xfer_len = last_xfer_size;
		else
			xfer_len = xfer->len;

		gb_xfer->speed_hz = cpu_to_le32(xfer->speed_hz);
		gb_xfer->len = cpu_to_le32(xfer_len);
		gb_xfer->delay_usecs = cpu_to_le16(xfer->delay_usecs);
		gb_xfer->cs_change = xfer->cs_change;
		gb_xfer->bits_per_word = xfer->bits_per_word;

		/* Copy tx data */
		if (xfer->tx_buf) {
			gb_xfer->rdwr |= GB_SPI_XFER_WRITE;
			memcpy(tx_data, xfer->tx_buf, xfer_len);
			tx_data += xfer_len;
		}

		if (xfer->rx_buf)
			gb_xfer->rdwr |= GB_SPI_XFER_READ;

		if (last_xfer && xfer == last_xfer)
			break;

		gb_xfer++;
	}

	return operation;
}

static void gb_spi_decode_response(struct spi_message *msg,
				   struct gb_spi_transfer_response *response)
{
	struct spi_transfer *xfer;
	void *rx_data = response->data;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		/* Copy rx data */
		if (xfer->rx_buf) {
			memcpy(xfer->rx_buf, rx_data, xfer->len);
			rx_data += xfer->len;
		}
	}
}

static int gb_spi_transfer_one_message(struct spi_master *master,
				       struct spi_message *msg)
{
	struct gb_spi *spi = spi_master_get_devdata(master);
	struct gb_connection *connection = spi->connection;
	struct gb_spi_transfer_response *response;
	struct gb_operation *operation;
	u32 len = 0;
	int ret;

	operation = gb_spi_operation_create(connection, msg, &len);
	if (!operation)
		return -ENOMEM;

	ret = gb_operation_request_send_sync(operation);
	if (!ret) {
		response = operation->response->payload;
		if (response)
			gb_spi_decode_response(msg, response);
	} else {
		pr_err("transfer operation failed (%d)\n", ret);
	}

	gb_operation_put(operation);

	msg->actual_length = len;
	msg->status = 0;
	spi_finalize_current_message(master);

	return ret;
}

static int gb_spi_setup(struct spi_device *spi)
{
	/* Nothing to do for now */
	return 0;
}

static void gb_spi_cleanup(struct spi_device *spi)
{
	/* Nothing to do for now */
}


/* Routines to get controller information */

/*
 * Map Greybus spi mode bits/flags/bpw into Linux ones.
 * All bits are same for now and so these macro's return same values.
 */
#define gb_spi_mode_map(mode) mode
#define gb_spi_flags_map(flags) flags

static int gb_spi_get_master_config(struct gb_spi *spi)
{
	struct gb_spi_master_config_response response;
	u16 mode, flags;
	int ret;

	ret = gb_operation_sync(spi->connection, GB_SPI_TYPE_MASTER_CONFIG,
				NULL, 0, &response, sizeof(response));
	if (ret < 0)
		return ret;

	mode = le16_to_cpu(response.mode);
	spi->mode = gb_spi_mode_map(mode);

	flags = le16_to_cpu(response.flags);
	spi->flags = gb_spi_flags_map(flags);

	spi->bits_per_word_mask = le32_to_cpu(response.bits_per_word_mask);
	spi->num_chipselect = response.num_chipselect;

	spi->min_speed_hz = le32_to_cpu(response.min_speed_hz);
	spi->max_speed_hz = le32_to_cpu(response.max_speed_hz);

	return 0;
}

static int gb_spi_setup_device(struct gb_spi *spi, u8 cs)
{
	struct spi_master *master = get_master_from_spi(spi);
	struct gb_spi_device_config_request request;
	struct gb_spi_device_config_response response;
	struct spi_board_info spi_board = { {0} };
	struct spi_device *spidev;
	int ret;

	request.chip_select = cs;

	ret = gb_operation_sync(spi->connection, GB_SPI_TYPE_DEVICE_CONFIG,
				&request, sizeof(request),
				&response, sizeof(response));
	if (ret < 0)
		return ret;

	memcpy(spi_board.modalias, response.name, sizeof(spi_board.modalias));
	spi_board.mode		= le16_to_cpu(response.mode);
	spi_board.bus_num	= master->bus_num;
	spi_board.chip_select	= cs;
	spi_board.max_speed_hz	= le32_to_cpu(response.max_speed_hz);

	spidev = spi_new_device(master, &spi_board);
	if (!spidev)
		ret = -EINVAL;

	return 0;
}

static int gb_spi_connection_init(struct gb_connection *connection)
{
	struct gb_spi *spi;
	struct spi_master *master;
	int ret;
	u8 i;

	/* Allocate master with space for data */
	master = spi_alloc_master(&connection->bundle->dev, sizeof(*spi));
	if (!master) {
		dev_err(&connection->bundle->dev, "cannot alloc SPI master\n");
		return -ENOMEM;
	}

	spi = spi_master_get_devdata(master);
	spi->connection = connection;
	connection->private = master;

	/* get master configuration */
	ret = gb_spi_get_master_config(spi);
	if (ret)
		goto out_put_master;

	master->bus_num = -1; /* Allow spi-core to allocate it dynamically */
	master->num_chipselect = spi->num_chipselect;
	master->mode_bits = spi->mode;
	master->flags = spi->flags;
	master->bits_per_word_mask = spi->bits_per_word_mask;

	/* Attach methods */
	master->cleanup = gb_spi_cleanup;
	master->setup = gb_spi_setup;
	master->transfer_one_message = gb_spi_transfer_one_message;

	ret = spi_register_master(master);
	if (ret < 0)
		goto out_put_master;

	/* now, fetch the devices configuration */
	for (i = 0; i < spi->num_chipselect; i++) {
		ret = gb_spi_setup_device(spi, i);
		if (ret < 0) {
			dev_err(&connection->bundle->dev,
				"failed to allocated spi device: %d\n", ret);
			spi_unregister_master(master);
			break;
		}
	}

	return ret;

out_put_master:
	spi_master_put(master);

	return ret;
}

static void gb_spi_connection_exit(struct gb_connection *connection)
{
	struct spi_master *master = connection->private;

	spi_unregister_master(master);
}

static struct gb_protocol spi_protocol = {
	.name			= "spi",
	.id			= GREYBUS_PROTOCOL_SPI,
	.major			= GB_SPI_VERSION_MAJOR,
	.minor			= GB_SPI_VERSION_MINOR,
	.connection_init	= gb_spi_connection_init,
	.connection_exit	= gb_spi_connection_exit,
	.request_recv		= NULL,
};

gb_builtin_protocol_driver(spi_protocol);
