/*
 * Greybus protocol handling
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#ifndef __PROTOCOL_H
#define __PROTOCOL_H

struct gb_connection;
struct gb_operation;

/* Possible flags for protocol drivers */
#define GB_PROTOCOL_SKIP_CONTROL_CONNECTED	BIT(0)	/* Don't sent connected requests */
#define GB_PROTOCOL_SKIP_CONTROL_DISCONNECTED	BIT(1)	/* Don't sent disconnected requests */
#define GB_PROTOCOL_SKIP_VERSION		BIT(3)	/* Don't send get_version() requests */

typedef int (*gb_connection_init_t)(struct gb_connection *);
typedef void (*gb_connection_exit_t)(struct gb_connection *);
typedef int (*gb_request_recv_t)(u8, struct gb_operation *);

/*
 * Protocols having the same id but different major and/or minor
 * version numbers are treated as distinct protocols.  If it makes
 * sense someday we could group protocols having the same id.
 */
struct gb_protocol {
	u8			id;
	u8			major;
	u8			minor;
	u8			count;
	unsigned long		flags;

	struct list_head	links;		/* global list */

	gb_connection_init_t	connection_init;
	gb_connection_exit_t	connection_exit;
	gb_request_recv_t	request_recv;
	struct module		*owner;
	char			*name;
};

int __gb_protocol_register(struct gb_protocol *protocol, struct module *module);
void gb_protocol_deregister(struct gb_protocol *protocol);

#define gb_protocol_register(protocol) \
	__gb_protocol_register(protocol, THIS_MODULE)

struct gb_protocol *gb_protocol_get(u8 id, u8 major, u8 minor);
struct gb_protocol *gb_protocol_get_latest(u8 id, u8 major, bool match);
int gb_protocol_get_version(struct gb_connection *connection);
int gb_protocol_version_negotiate(struct gb_connection *connection);

void gb_protocol_put(struct gb_protocol *protocol);

/*
 * These are defined in their respective protocol source files.
 * Declared here for now.  They could be added via modules, or maybe
 * just use initcalls (which level?).
 */
extern int gb_gpio_protocol_init(void);
extern void gb_gpio_protocol_exit(void);

extern int gb_pwm_protocol_init(void);
extern void gb_pwm_protocol_exit(void);

extern int gb_uart_protocol_init(void);
extern void gb_uart_protocol_exit(void);

extern int gb_sdio_protocol_init(void);
extern void gb_sdio_protocol_exit(void);

extern int gb_usb_protocol_init(void);
extern void gb_usb_protocol_exit(void);

extern int gb_i2c_protocol_init(void);
extern void gb_i2c_protocol_exit(void);

extern int gb_spi_protocol_init(void);
extern void gb_spi_protocol_exit(void);

/* __protocol: Pointer to struct gb_protocol */
#define gb_protocol_driver(__protocol)			\
static int __init protocol_init(void)			\
{							\
	return gb_protocol_register(__protocol);	\
}							\
module_init(protocol_init);				\
static void __exit protocol_exit(void)			\
{							\
	gb_protocol_deregister(__protocol);		\
}							\
module_exit(protocol_exit)

/* __protocol: string matching name of struct gb_protocol */
#define gb_builtin_protocol_driver(__protocol)		\
int __init gb_##__protocol##_init(void)			\
{							\
	return gb_protocol_register(&__protocol);	\
}							\
void gb_##__protocol##_exit(void)			\
{							\
	gb_protocol_deregister(&__protocol);		\
}							\

#endif /* __PROTOCOL_H */
