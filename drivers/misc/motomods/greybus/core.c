/*
 * Greybus "Core"
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define CREATE_TRACE_POINTS
#include "greybus.h"
#include "greybus_trace.h"

EXPORT_TRACEPOINT_SYMBOL_GPL(gb_host_device_send);
EXPORT_TRACEPOINT_SYMBOL_GPL(gb_host_device_recv);

/* Allow greybus to be disabled at boot if needed */
static bool nogreybus;
#ifdef MODULE
module_param(nogreybus, bool, 0444);
#else
core_param(nogreybus, nogreybus, bool, 0444);
#endif
int greybus_disabled(void)
{
	return nogreybus;
}
EXPORT_SYMBOL_GPL(greybus_disabled);

static int greybus_match_one_id(struct gb_bundle *bundle,
				     const struct greybus_bundle_id *id)
{
	if ((id->match_flags & GREYBUS_ID_MATCH_VENDOR) &&
	    (id->vendor != bundle->intf->vendor_id))
		return 0;

	if ((id->match_flags & GREYBUS_ID_MATCH_PRODUCT) &&
	    (id->product != bundle->intf->product_id))
		return 0;

	if ((id->match_flags & GREYBUS_ID_MATCH_CLASS) &&
	    (id->class != bundle->class))
		return 0;

	return 1;
}

static const struct greybus_bundle_id *
greybus_match_id(struct gb_bundle *bundle, const struct greybus_bundle_id *id)
{
	if (id == NULL)
		return NULL;

	for (; id->vendor || id->product || id->class || id->driver_info;
									id++) {
		if (greybus_match_one_id(bundle, id))
			return id;
	}

	return NULL;
}

static int greybus_module_match(struct device *dev, struct device_driver *drv)
{
	struct greybus_driver *driver = to_greybus_driver(drv);
	struct gb_bundle *bundle = to_gb_bundle(dev);
	const struct greybus_bundle_id *id;

	id = greybus_match_id(bundle, driver->id_table);
	if (id)
		return 1;
	/* FIXME - Dynamic ids? */
	return 0;
}

static int greybus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct gb_host_device *hd;
	struct gb_interface *intf = NULL;
	struct gb_bundle *bundle = NULL;
	struct gb_svc *svc = NULL;

	if (is_gb_host_device(dev)) {
		hd = to_gb_host_device(dev);
	} else if (is_gb_interface(dev)) {
		intf = to_gb_interface(dev);
		hd = intf->hd;
	} else if (is_gb_bundle(dev)) {
		bundle = to_gb_bundle(dev);
		intf = bundle->intf;
		hd = intf->hd;
	} else if (is_gb_svc(dev)) {
		svc = to_gb_svc(dev);
		hd = svc->hd;
	} else {
		dev_WARN(dev, "uevent for unknown greybus device \"type\"!\n");
		return -EINVAL;
	}

	if (add_uevent_var(env, "BUS=%u", hd->bus_id))
		return -ENOMEM;

	if (intf) {
		if (add_uevent_var(env, "INTERFACE=%u", intf->interface_id))
			return -ENOMEM;
	}

	if (bundle) {
		// FIXME
		// add a uevent that can "load" a bundle type
		// This is what we need to bind a driver to so use the info
		// in gmod here as well

		if (add_uevent_var(env, "BUNDLE=%u", bundle->id))
			return -ENOMEM;
	}

	return 0;
}

struct bus_type greybus_bus_type = {
	.name =		"greybus",
	.match =	greybus_module_match,
	.uevent =	greybus_uevent,
};

static int greybus_probe(struct device *dev)
{
	struct greybus_driver *driver = to_greybus_driver(dev->driver);
	struct gb_bundle *bundle = to_gb_bundle(dev);
	const struct greybus_bundle_id *id;
	int retval;

	/* match id */
	id = greybus_match_id(bundle, driver->id_table);
	if (!id)
		return -ENODEV;

	retval = driver->probe(bundle, id);
	if (retval)
		return retval;

	return 0;
}

static int greybus_remove(struct device *dev)
{
	struct greybus_driver *driver = to_greybus_driver(dev->driver);
	struct gb_bundle *bundle = to_gb_bundle(dev);

	driver->disconnect(bundle);
	return 0;
}

int greybus_register_driver(struct greybus_driver *driver, struct module *owner,
		const char *mod_name)
{
	int retval;

	if (greybus_disabled())
		return -ENODEV;

	driver->driver.name = driver->name;
	driver->driver.probe = greybus_probe;
	driver->driver.remove = greybus_remove;
	driver->driver.owner = owner;
	driver->driver.mod_name = mod_name;

	retval = driver_register(&driver->driver);
	if (retval)
		return retval;

	pr_info("registered new driver %s\n", driver->name);
	return 0;
}
EXPORT_SYMBOL_GPL(greybus_register_driver);

void greybus_deregister_driver(struct greybus_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL_GPL(greybus_deregister_driver);

static int __init gb_init(void)
{
	int retval;

	if (greybus_disabled())
		return -ENODEV;

	BUILD_BUG_ON(CPORT_ID_MAX >= (long)CPORT_ID_BAD);

	gb_debugfs_init();

	retval = bus_register(&greybus_bus_type);
	if (retval) {
		pr_err("bus_register failed (%d)\n", retval);
		goto error_bus;
	}

	retval = gb_hd_init();
	if (retval) {
		pr_err("gb_hd_init failed (%d)\n", retval);
		goto error_hd;
	}

	retval = gb_operation_init();
	if (retval) {
		pr_err("gb_operation_init failed (%d)\n", retval);
		goto error_operation;
	}

	retval = gb_control_protocol_init();
	if (retval) {
		pr_err("gb_control_protocol_init failed\n");
		goto error_control;
	}

	retval = gb_svc_protocol_init();
	if (retval) {
		pr_err("gb_svc_protocol_init failed\n");
		goto error_svc;
	}

	retval = gb_firmware_protocol_init();
	if (retval) {
		pr_err("gb_firmware_protocol_init failed\n");
		goto error_firmware;
	}

	return 0;	/* Success */

error_firmware:
	gb_svc_protocol_exit();
error_svc:
	gb_control_protocol_exit();
error_control:
	gb_operation_exit();
error_operation:
	gb_hd_exit();
error_hd:
	bus_unregister(&greybus_bus_type);
error_bus:
	gb_debugfs_cleanup();

	return retval;
}
module_init(gb_init);

static void __exit gb_exit(void)
{
	gb_firmware_protocol_exit();
	gb_svc_protocol_exit();
	gb_control_protocol_exit();
	gb_operation_exit();
	gb_hd_exit();
	bus_unregister(&greybus_bus_type);
	gb_debugfs_cleanup();
	tracepoint_synchronize_unregister();
}
module_exit(gb_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Greg Kroah-Hartman <gregkh@linuxfoundation.org>");
