################################################################################
#
# sample_daemon
#
################################################################################

ifeq    ($(BR2_BUILD_SAMPLE_DAEMON),y)

SAMPLE_DAEMON_VERSION = 0.1
SAMPLE_DAEMON_SITE = \
  "${BR2_EXTERNAL_piksi_buildroot_PATH}/package/sample_daemon"
SAMPLE_DAEMON_SITE_METHOD = local
SAMPLE_DAEMON_DEPENDENCIES = czmq libsbp libpiksi

define SAMPLE_DAEMON_INVOKE_MAKE
    $(MAKE) CROSS=$(TARGET_CROSS) LD=$(TARGET_LD) -C $(@D)/src all
endef

define SAMPLE_DAEMON_INVOKE_INSTALL
    $(INSTALL) -D -m 0755 $(@D)/src/sample_daemon $(TARGET_DIR)/usr/bin
endef

define SAMPLE_DAEMON_BUILD_CMDS
	$(SAMPLE_DAEMON_INVOKE_MAKE)
endef

define SAMPLE_DAEMON_INSTALL_TARGET_CMDS
	$(SAMPLE_DAEMON_INVOKE_INSTALL)
endef

$(eval $(generic-package))

BR2_ROOTFS_OVERLAY += "${SAMPLE_DAEMON_SITE}/overlay"

endif # ($(BR2_BUILD_SAMPLE_DAEMON),y)
