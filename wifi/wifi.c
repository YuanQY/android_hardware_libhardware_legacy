/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#ifdef USES_TI_MAC80211
#include <dirent.h>
#include <net/if.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include "nl80211.h"
#endif

#include "hardware_legacy/wifi.h"
#include "libwpa_client/wpa_ctrl.h"

#define LOG_TAG "WifiHW"
#define DBG 1
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
// Engle add for MTK, start
#ifdef TARGET_MTK
#include <cutils/sockets.h>
#include <dlfcn.h>
#endif
// Engle add for MTK, end
#include "private/android_filesystem_config.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif

static struct wpa_ctrl *ctrl_conn;
static struct wpa_ctrl *monitor_conn;

/* socket pair used to exit from a blocking read */
static int exit_sockets[2];

extern int do_dhcp();
extern int ifc_init();
extern void ifc_close();
extern char *dhcp_lasterror();
extern void get_dhcp_info();
extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);
void wifi_close_sockets();

static int wifi_mode = 0;

static char primary_iface[PROPERTY_VALUE_MAX];
// TODO: use new ANDROID_SOCKET mechanism, once support for multiple
// sockets is in

#ifdef USES_TI_MAC80211
#define P2P_INTERFACE            "p2p0"
struct nl_sock *nl_soc;
struct nl_cache *nl_cache;
struct genl_family *nl80211;
#endif

#ifdef TARGET_MTK
#define P2P_INTERFACE            "p2p0"
#define AP_INTERFACE             "ap0"
#endif

#ifndef WIFI_DRIVER_MODULE_ARG
#define WIFI_DRIVER_MODULE_ARG          ""
#endif
#ifndef WIFI_DRIVER_MODULE_AP_ARG
#define WIFI_DRIVER_MODULE_AP_ARG       ""
#endif
#ifndef WIFI_FIRMWARE_LOADER
#define WIFI_FIRMWARE_LOADER        ""
#endif
#define WIFI_TEST_INTERFACE        "sta"

#ifndef WIFI_DRIVER_FW_PATH_STA
#define WIFI_DRIVER_FW_PATH_STA        NULL
#endif
#ifndef WIFI_DRIVER_FW_PATH_AP
#define WIFI_DRIVER_FW_PATH_AP        NULL
#endif
#ifndef WIFI_DRIVER_FW_PATH_P2P
#define WIFI_DRIVER_FW_PATH_P2P        NULL
#endif

#ifdef WIFI_EXT_MODULE_NAME
static const char EXT_MODULE_NAME[] = WIFI_EXT_MODULE_NAME;
#ifdef WIFI_EXT_MODULE_ARG
static const char EXT_MODULE_ARG[] = WIFI_EXT_MODULE_ARG;
#else
static const char EXT_MODULE_ARG[] = "";
#endif
#endif
#ifdef WIFI_EXT_MODULE_PATH
static const char EXT_MODULE_PATH[] = WIFI_EXT_MODULE_PATH;
#endif

#ifndef WIFI_DRIVER_FW_PATH_PARAM
#define WIFI_DRIVER_FW_PATH_PARAM    "/sys/module/wlan/parameters/fwpath"
#endif

// Engle, add for MTK, start
#ifdef TARGET_MTK
static const char IFACE_DIR[]           = "/data/misc/wpa_supplicant";
#else
static const char IFACE_DIR[]           = "/data/system/wpa_supplicant";
#endif
// Engle, add for MTK, end

#ifdef WIFI_DRIVER_MODULE_PATH
static const char DRIVER_MODULE_NAME[]  = WIFI_DRIVER_MODULE_NAME;
static const char DRIVER_MODULE_TAG[]   = WIFI_DRIVER_MODULE_NAME " ";
static const char DRIVER_MODULE_PATH[]  = WIFI_DRIVER_MODULE_PATH;
static const char DRIVER_MODULE_ARG[]   = WIFI_DRIVER_MODULE_ARG;
static const char DRIVER_MODULE_AP_ARG[] = WIFI_DRIVER_MODULE_AP_ARG;
#endif
static const char FIRMWARE_LOADER[]     = WIFI_FIRMWARE_LOADER;
static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";
static const char SUPPLICANT_NAME[]     = "wpa_supplicant";
static const char SUPP_PROP_NAME[]      = "init.svc.wpa_supplicant";
static const char P2P_SUPPLICANT_NAME[] = "p2p_supplicant";
static const char P2P_PROP_NAME[]       = "init.svc.p2p_supplicant";
static const char AP_SUPPLICANT_NAME[]  = "ap_daemon";
static const char AP_PROP_NAME[]        = "init.svc.ap_daemon";
static const char SUPP_CONFIG_TEMPLATE[]= "/system/etc/wifi/wpa_supplicant.conf";
static const char SUPP_CONFIG_FILE[]    = "/data/misc/wifi/wpa_supplicant.conf";
static const char P2P_CONFIG_FILE[]     = "/data/misc/wifi/p2p_supplicant.conf";
static const char CONTROL_IFACE_PATH[]  = "/data/misc/wifi/sockets";
static const char MODULE_FILE[]         = "/proc/modules";

#define WIFI_POWER_PATH     "/dev/wmtWifi"

static const char IFNAME[]              = "IFNAME=";
#define IFNAMELEN            (sizeof(IFNAME) - 1)
static const char WPA_EVENT_IGNORE[]    = "CTRL-EVENT-IGNORE ";

static const char SUPP_ENTROPY_FILE[]   = WIFI_ENTROPY_FILE;
static unsigned char dummy_key[21] = { 0x02, 0x11, 0xbe, 0x33, 0x43, 0x35,
                                       0x68, 0x47, 0x84, 0x99, 0xa9, 0x2b,
                                       0x1c, 0xd3, 0xee, 0xff, 0xf1, 0xe2,
                                       0xf3, 0xf4, 0xf5 };

/* Is either SUPPLICANT_NAME or P2P_SUPPLICANT_NAME */
static char supplicant_name[PROPERTY_VALUE_MAX] = {0};
/* Is either SUPP_PROP_NAME or P2P_PROP_NAME */
static char supplicant_prop_name[PROPERTY_KEY_MAX] = {0};


#ifdef SAMSUNG_WIFI
char* get_samsung_wifi_type()
{
    char buf[10];
    int fd = open("/data/.cid.info", O_RDONLY);
    if (fd < 0)
        return NULL;

    if (read(fd, buf, sizeof(buf)) < 0) {
        close(fd);
        return NULL;
    }

    close(fd);

    if (strncmp(buf, "murata", 6) == 0)
        return "_murata";

    if (strncmp(buf, "semcove", 7) == 0)
        return "_semcove";

    if (strncmp(buf, "semcosh", 7) == 0)
        return "_semcosh";

    return NULL;
}
#endif

static int insmod(const char *filename, const char *args)
{
    void *module;
    unsigned int size;
    int ret;

    module = load_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, args);

    free(module);

    return ret;
}

static int rmmod(const char *modname)
{
    int ret = -1;
    int maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);
        if (ret < 0 && errno == EAGAIN)
            usleep(500000);
        else
            break;
    }

    if (ret != 0)
        ALOGD("Unable to unload driver module \"%s\": %s\n",
             modname, strerror(errno));
    return ret;
}

int do_dhcp_request(int *ipaddr, int *gateway, int *mask,
                    int *dns1, int *dns2, int *server, int *lease) {
    /* For test driver, always report success */
    if (strcmp(primary_iface, WIFI_TEST_INTERFACE) == 0)
        return 0;

    if (ifc_init() < 0)
        return -1;

    if (do_dhcp(primary_iface) < 0) {
        ifc_close();
        return -1;
    }
    ifc_close();
    get_dhcp_info(ipaddr, gateway, mask, dns1, dns2, server, lease);
    return 0;
}

const char *get_dhcp_error_string() {
    return dhcp_lasterror();
}

// Engle, add for MTK, start
#ifdef TARGET_MTK

int is_wifi_driver_loaded() {
    char driver_status[PROPERTY_VALUE_MAX];

    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);

    if (!property_get(DRIVER_PROP_NAME, driver_status, NULL)
            || strcmp(driver_status, "ok") != 0) {
        if (DBG)
            ALOGD("Check driver status: not loaded");

        return 0;  /* driver not loaded */
    }

    if (DBG)
        ALOGD("Check driver status: loaded");
    return 1;
}

#else // NOT TARGET_MTK

int is_wifi_driver_loaded() {
    char driver_status[PROPERTY_VALUE_MAX];
#ifdef WIFI_DRIVER_MODULE_PATH
    FILE *proc;
    char line[sizeof(DRIVER_MODULE_TAG)+10];
#endif

    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);

    if (!property_get(DRIVER_PROP_NAME, driver_status, NULL)
            || strcmp(driver_status, "ok") != 0) {
        return 0;  /* driver not loaded */
    }
#ifdef WIFI_DRIVER_MODULE_PATH
    /*
     * If the property says the driver is loaded, check to
     * make sure that the property setting isn't just left
     * over from a previous manual shutdown or a runtime
     * crash.
     */
    if ((proc = fopen(MODULE_FILE, "r")) == NULL) {
        ALOGW("Could not open %s: %s", MODULE_FILE, strerror(errno));
        property_set(DRIVER_PROP_NAME, "unloaded");
        return 0;
    }
    while ((fgets(line, sizeof(line), proc)) != NULL) {
        if (strncmp(line, DRIVER_MODULE_TAG, strlen(DRIVER_MODULE_TAG)) == 0) {
            fclose(proc);
            return 1;
        }
    }
    fclose(proc);
    property_set(DRIVER_PROP_NAME, "unloaded");
    return 0;
#else
    return 1;
#endif
}

#endif
// Engle, add for MTK, end

int wifi_load_driver()
{
    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);
    wifi_set_power(1);
    property_set(DRIVER_PROP_NAME, "ok");
    return 0;
}

int wifi_unload_driver()
{
    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);
    if (is_wifi_driver_loaded()) {
        wifi_set_p2p_mod(0, 0);
        wifi_set_power(0);
        property_set(DRIVER_PROP_NAME, "unloaded");
    }
    return 0;
}

int ensure_entropy_file_exists()
{
    int ret;
    int destfd;

    ret = access(SUPP_ENTROPY_FILE, R_OK|W_OK);
    if ((ret == 0) || (errno == EACCES)) {
        if ((ret != 0) &&
            (chmod(SUPP_ENTROPY_FILE, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) != 0)) {
            ALOGE("Cannot set RW to \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
            return -1;
        }
        return 0;
    }
    destfd = TEMP_FAILURE_RETRY(open(SUPP_ENTROPY_FILE, O_CREAT|O_RDWR, 0660));
    if (destfd < 0) {
        ALOGE("Cannot create \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        return -1;
    }

    if (TEMP_FAILURE_RETRY(write(destfd, dummy_key, sizeof(dummy_key))) != sizeof(dummy_key)) {
        ALOGE("Error writing \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        close(destfd);
        return -1;
    }
    close(destfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(SUPP_ENTROPY_FILE, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
             SUPP_ENTROPY_FILE, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }

    if (chown(SUPP_ENTROPY_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
             SUPP_ENTROPY_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }
    return 0;
}

int update_ctrl_interface(const char *config_file) {

    int srcfd, destfd;
    int nread;
    char ifc[PROPERTY_VALUE_MAX];
    char *pbuf;
    char *sptr;
    struct stat sb;
    int ret;

    if (DBG)
        ALOGD("%s:%d enter config file \"%s\"", __FUNCTION__, __LINE__, config_file);

    if (stat(config_file, &sb) != 0)
        return -1;

    pbuf = malloc(sb.st_size + PROPERTY_VALUE_MAX);
    if (!pbuf)
        return 0;
    srcfd = TEMP_FAILURE_RETRY(open(config_file, O_RDONLY));
    if (srcfd < 0) {
        ALOGE("Cannot open \"%s\": %s", config_file, strerror(errno));
        free(pbuf);
        return 0;
    }
    nread = TEMP_FAILURE_RETRY(read(srcfd, pbuf, sb.st_size));
    close(srcfd);
    if (nread < 0) {
        ALOGE("Cannot read \"%s\": %s", config_file, strerror(errno));
        free(pbuf);
        return 0;
    }

// Engle, add for MTK, start
#ifdef TARGET_MTK
    strcpy(ifc, IFACE_DIR);
#else
    if (!strcmp(config_file, SUPP_CONFIG_FILE)) {
        property_get("wifi.interface", ifc, WIFI_TEST_INTERFACE);
    } else {
        strcpy(ifc, CONTROL_IFACE_PATH);
    }
#endif
// Engle, add for MTK, end

    /* Assume file is invalid to begin with */
    ret = -1;
    /*
     * if there is a "ctrl_interface=<value>" entry, re-write it ONLY if it is
     * NOT a directory.  The non-directory value option is an Android add-on
     * that allows the control interface to be exchanged through an environment
     * variable (initialized by the "init" program when it starts a service
     * with a "socket" option).
     *
     * The <value> is deemed to be a directory if the "DIR=" form is used or
     * the value begins with "/".
     */
    if ((sptr = strstr(pbuf, "ctrl_interface="))) {
        ret = 0;
        if ((!strstr(pbuf, "ctrl_interface=DIR=")) &&
                (!strstr(pbuf, "ctrl_interface=/"))) {
            char *iptr = sptr + strlen("ctrl_interface=");
            int ilen = 0;
            int mlen = strlen(ifc);
            int nwrite;
            if (strncmp(ifc, iptr, mlen) != 0) {
                ALOGE("ctrl_interface != %s", ifc);
                while (((ilen + (iptr - pbuf)) < nread) && (iptr[ilen] != '\n'))
                    ilen++;
                mlen = ((ilen >= mlen) ? ilen : mlen) + 1;
                memmove(iptr + mlen, iptr + ilen + 1, nread - (iptr + ilen + 1 - pbuf));
                memset(iptr, '\n', mlen);
                memcpy(iptr, ifc, strlen(ifc));
                destfd = TEMP_FAILURE_RETRY(open(config_file, O_RDWR, 0660));
                if (destfd < 0) {
                    ALOGE("Cannot update \"%s\": %s", config_file, strerror(errno));
                    free(pbuf);
                    return -1;
                }
                TEMP_FAILURE_RETRY(write(destfd, pbuf, nread + mlen - ilen -1));
                close(destfd);
            }
        }
    }
    free(pbuf);
    return ret;
}

int ensure_config_file_exists(const char *config_file)
{
    char buf[2048];
    int srcfd, destfd;
    struct stat sb;
    int nread;
    int ret;

    ret = access(config_file, R_OK|W_OK);
    if ((ret == 0) || (errno == EACCES)) {
        if ((ret != 0) &&
            (chmod(config_file, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) != 0)) {
            ALOGE("Cannot set RW to \"%s\": %s", config_file, strerror(errno));
            return -1;
        }
        /* return if we were able to update control interface properly */
        if (update_ctrl_interface(config_file) >=0) {
            return 0;
        } else {
            /* This handles the scenario where the file had bad data
             * for some reason. We continue and recreate the file.
             */
        }
    } else if (errno != ENOENT) {
        ALOGE("Cannot access \"%s\": %s", config_file, strerror(errno));
        return -1;
    }

    srcfd = TEMP_FAILURE_RETRY(open(SUPP_CONFIG_TEMPLATE, O_RDONLY));
    if (srcfd < 0) {
        ALOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = TEMP_FAILURE_RETRY(open(config_file, O_CREAT|O_RDWR, 0660));
    if (destfd < 0) {
        close(srcfd);
        ALOGE("Cannot create \"%s\": %s", config_file, strerror(errno));
        return -1;
    }

    while ((nread = TEMP_FAILURE_RETRY(read(srcfd, buf, sizeof(buf)))) != 0) {
        if (nread < 0) {
            ALOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(config_file);
            return -1;
        }
        TEMP_FAILURE_RETRY(write(destfd, buf, nread));
    }

    close(destfd);
    close(srcfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(config_file, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
             config_file, strerror(errno));
        unlink(config_file);
        return -1;
    }

    if (chown(config_file, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
             config_file, AID_WIFI, strerror(errno));
        unlink(config_file);
        return -1;
    }
    return update_ctrl_interface(config_file);
}

#ifdef USES_TI_MAC80211
static int init_nl()
{
    int err;

    nl_soc = nl_socket_alloc();
    if (!nl_soc) {
        ALOGE("Failed to allocate netlink socket.");
        return -ENOMEM;
    }

    if (genl_connect(nl_soc)) {
        ALOGE("Failed to connect to generic netlink.");
        err = -ENOLINK;
        goto out_handle_destroy;
    }

    genl_ctrl_alloc_cache(nl_soc, &nl_cache);
    if (!nl_cache) {
        ALOGE("Failed to allocate generic netlink cache.");
        err = -ENOMEM;
        goto out_handle_destroy;
    }

    nl80211 = genl_ctrl_search_by_name(nl_cache, "nl80211");
    if (!nl80211) {
        ALOGE("nl80211 not found.");
        err = -ENOENT;
        goto out_cache_free;
    }

    return 0;

out_cache_free:
    nl_cache_free(nl_cache);
out_handle_destroy:
    nl_socket_free(nl_soc);
    return err;
}

static void deinit_nl()
{
    genl_family_put(nl80211);
    nl_cache_free(nl_cache);
    nl_socket_free(nl_soc);
}

// ignore the "." and ".." entries
static int dir_filter(const struct dirent *name)
{
    if (0 == strcmp("..", name->d_name) ||
        0 == strcmp(".", name->d_name))
            return 0;

    return 1;
}

// lookup the only active phy
int phy_lookup()
{
    char buf[200];
    int fd, pos;
    struct dirent **namelist;
    int n, i;

    n = scandir("/sys/class/ieee80211", &namelist, dir_filter,
                (int (*)(const struct dirent**, const struct dirent**))alphasort);
    if (n != 1) {
        ALOGE("unexpected - found %d phys in /sys/class/ieee80211", n);
        for (i = 0; i < n; i++)
            free(namelist[i]);
        if (n > 0)
            free(namelist);
        return -1;
    }

    snprintf(buf, sizeof(buf), "/sys/class/ieee80211/%s/index",
             namelist[0]->d_name);
    free(namelist[0]);
    free(namelist);

    fd = open(buf, O_RDONLY);
    if (fd < 0)
        return -1;
    pos = read(fd, buf, sizeof(buf) - 1);
    if (pos < 0) {
        close(fd);
        return -1;
    }
    buf[pos] = '\0';
    close(fd);
    return atoi(buf);
}

int nl_error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
    int *ret = (int *)arg;
    *ret = err->error;
    return NL_STOP;
}

int nl_finish_handler(struct nl_msg *msg, void *arg)
{
     int *ret = (int *)arg;
     *ret = 0;
     return NL_SKIP;
}

int nl_ack_handler(struct nl_msg *msg, void *arg)
{
    int *ret = (int *)arg;
    *ret = 0;
    return NL_STOP;
}

static int execute_nl_interface_cmd(const char *iface,
                                    enum nl80211_iftype type,
                                    uint8_t cmd)
{
    struct nl_cb *cb;
    struct nl_msg *msg;
    int devidx = 0;
    int err;
    int add_interface = (cmd == NL80211_CMD_NEW_INTERFACE);

    if (DBG)
        ALOGD("%s:%d enter iface[%s]", __FUNCTION__, __LINE__, iface);

    if (add_interface) {
        devidx = phy_lookup();
    } else {
        devidx = if_nametoindex(iface);
        if (devidx == 0) {
            ALOGE("failed to translate ifname to idx");
            return -errno;
        }
    }

    msg = nlmsg_alloc();
    if (!msg) {
        ALOGE("failed to allocate netlink message");
        return 2;
    }

    cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        ALOGE("failed to allocate netlink callbacks");
        err = 2;
        goto out_free_msg;
    }

    genlmsg_put(msg, 0, 0, genl_family_get_id(nl80211), 0, 0, cmd, 0);

    if (add_interface) {
        NLA_PUT_U32(msg, NL80211_ATTR_WIPHY, devidx);
    } else {
        NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
    }

    if (add_interface) {
        NLA_PUT_STRING(msg, NL80211_ATTR_IFNAME, iface);
        NLA_PUT_U32(msg, NL80211_ATTR_IFTYPE, type);
    }

    err = nl_send_auto_complete(nl_soc, msg);
    if (err < 0)
        goto out;

    err = 1;

    nl_cb_err(cb, NL_CB_CUSTOM, nl_error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nl_finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, nl_ack_handler, &err);

    while (err > 0)
        nl_recvmsgs(nl_soc, cb);
out:
    nl_cb_put(cb);
out_free_msg:
    nlmsg_free(msg);
    return err;
nla_put_failure:
    ALOGW("building message failed");
    return 2;
}

int add_remove_p2p_interface(int add)
{
    int ret;

    if (DBG)
        ALOGD("%s:%d enter [%d]", __FUNCTION__, __LINE__, add);

    ret = init_nl();
    if (ret != 0)
        return ret;

    if (add) {
        ret = execute_nl_interface_cmd(P2P_INTERFACE, NL80211_IFTYPE_STATION,
                                       NL80211_CMD_NEW_INTERFACE);
        if (ret != 0) {
            ALOGE("could not add P2P interface: %d", ret);
            goto cleanup;
        }
    } else {
        ret = execute_nl_interface_cmd(P2P_INTERFACE, NL80211_IFTYPE_STATION,
                                       NL80211_CMD_DEL_INTERFACE);
        if (ret != 0) {
            ALOGE("could not remove P2P interface: %d", ret);
            goto cleanup;
        }
    }

    ALOGD("added/removed p2p interface. add: %d", add);

cleanup:
    deinit_nl();
    return ret;
}
#endif /* USES_TI_MAC80211 */

// Engle add for MTK, start
#ifdef TARGET_MTK
int wifi_start_supplicant(int supplicantType)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0, i;
#endif
    void *handle = dlopen("/system/lib/libpalwlan_mtk.so", RTLD_LAZY);
    int ret = 0;
    typedef void (*pal_set_wlan_up_t)();
    typedef void (*pal_send_wlan_on_event_t)();
    if (DBG)
        ALOGD(" wifi_start_supplicant [%d]", supplicantType);

    switch(supplicantType) {
        case SUPPLICANT_STA:
            strcpy(supplicant_name, SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, SUPP_PROP_NAME);
            break;
        case SUPPLICANT_P2P:
            strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, P2P_PROP_NAME);
            break;
        case SUPPLICANT_AP:
            strcpy(supplicant_name, AP_SUPPLICANT_NAME);
            strcpy(supplicant_prop_name, AP_PROP_NAME);
            break;
        default:
            ALOGE("Unkown supplicant type [%d]", supplicantType);
            ret = -1;
            goto out;
    }
    
    if (supplicantType) {
         /* Ensure p2p config file is created */
        if (ensure_config_file_exists(P2P_CONFIG_FILE) < 0) {
            ALOGE("Failed to create a p2p config file");
            ret = -1;
            goto out;
        }        
    }

    /* Check whether already running */
    if (property_get(supplicant_name, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        if (DBG)
            ALOGD("%s:%d get %s status %s", __FUNCTION__, __LINE__, supplicant_name, supp_status);
        ret = 0;
        goto out;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists(SUPP_CONFIG_FILE) < 0) {
        ALOGE("Wi-Fi will not be enabled");
        ret = -1;
        goto out;
    }

    if (ensure_entropy_file_exists() < 0) {
        ALOGE("Wi-Fi entropy file was not created");
    }

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

    /* Reset sockets used for exiting from hung state */
    exit_sockets[0] = exit_sockets[1] = -1;

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(supplicant_prop_name);
    if (pi != NULL) {
        serial = __system_property_serial(pi);
    }
#endif

    switch(supplicantType) {
        case SUPPLICANT_STA:
            property_get("wifi.interface", primary_iface, WIFI_TEST_INTERFACE);
            break;
        case SUPPLICANT_P2P:
            property_get("wifi.direct.interface", primary_iface, WIFI_TEST_INTERFACE);
            break;
        case SUPPLICANT_AP:
            property_get("wifi.tethering.interface", primary_iface, WIFI_TEST_INTERFACE);
            break;
        default:
            property_get("wifi.interface", primary_iface, WIFI_TEST_INTERFACE);
            break;
    }
    
    property_set("ctl.start", supplicant_name);

    if (DBG)
        ALOGD("%s:%d set ctl.start to %s", __FUNCTION__, __LINE__, supplicant_name);

    sched_yield();

    if (DBG)
        ALOGD("%s:%d After call sched_yield", __FUNCTION__, __LINE__);

    while (count-- > 0) {
       if (DBG)
           ALOGD("%s:%d Before get %s status", __FUNCTION__, __LINE__, supplicant_prop_name);
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(supplicant_prop_name);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                ret = 0;
                goto out;
            } else if (__system_property_serial(pi) != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                ret = -1;
                goto out;
            }
        }
#else
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (DBG)
                ALOGD("%s:%d get %s status %s", __FUNCTION__, __LINE__, supplicant_prop_name, supp_status);

            if (strcmp(supp_status, "running") == 0) {
                ret = 0;
                goto out;
            }
        }
#endif
        usleep(100000);
    }
    ret = -1;
out:
   if (handle != NULL) {
       if (0 == ret) {
            pal_set_wlan_up_t pal_set_wlan_up = (pal_set_wlan_up_t)dlsym(handle, "pal_set_wlan_up");
            if (NULL == pal_set_wlan_up)
                ALOGE("Map pal_set_wlan_up error (%s)", dlerror());
            else
                  pal_set_wlan_up();
            pal_send_wlan_on_event_t pal_send_wlan_on_event = (pal_send_wlan_on_event_t)dlsym(handle, "pal_send_wlan_on_event");
            if (NULL == pal_send_wlan_on_event)
                ALOGE("Map pal_send_wlan_on_event error (%s)", dlerror());
            else
                pal_send_wlan_on_event();
            if (DBG && pal_set_wlan_up != NULL && pal_send_wlan_on_event != NULL)
                    ALOGD("[PAL] wifi_start_supplicant pass\n");
        }
        dlclose(handle);
        
    }
    return ret;
}

#else

int wifi_start_supplicant(int p2p_supported)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0, i;
#endif

    if (DBG)
        ALOGD("%s:%d Enter,  p2p_supported %d", __FUNCTION__, __LINE__, p2p_supported);

    if (p2p_supported) {
        strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
        strcpy(supplicant_prop_name, P2P_PROP_NAME);

        /* Ensure p2p config file is created */
        if (ensure_config_file_exists(P2P_CONFIG_FILE) < 0) {
            ALOGE("Failed to create a p2p config file");
            return -1;
        }

    } else {
        strcpy(supplicant_name, SUPPLICANT_NAME);
        strcpy(supplicant_prop_name, SUPP_PROP_NAME);
    }

    /* Check whether already running */
    if (property_get(supplicant_name, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        if (DBG)
            ALOGD("%s:%d get %s status %s", __FUNCTION__, __LINE__, supplicant_name, supp_status);
        return 0;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists(SUPP_CONFIG_FILE) < 0) {
        ALOGE("Wi-Fi will not be enabled");
        return -1;
    }

    if (ensure_entropy_file_exists() < 0) {
        ALOGE("Wi-Fi entropy file was not created");
    }

#ifdef USES_TI_MAC80211
    if (p2p_supported && add_remove_p2p_interface(1) < 0) {
        ALOGE("Wi-Fi - could not create p2p interface");
        return -1;
    }
#endif

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

    /* Reset sockets used for exiting from hung state */
    exit_sockets[0] = exit_sockets[1] = -1;

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(supplicant_prop_name);
    if (pi != NULL) {
        serial = __system_property_serial(pi);
    }
#endif
    property_get("wifi.interface", primary_iface, WIFI_TEST_INTERFACE);

    property_set("ctl.start", supplicant_name);
    if (DBG)
        ALOGD("%s:%d set ctl.start to %s", __FUNCTION__, __LINE__, supplicant_name);
    sched_yield();
    if (DBG)
        ALOGD("%s:%d After call sched_yield", __FUNCTION__, __LINE__);

    while (count-- > 0) {
       if (DBG)
           ALOGD("%s:%d Before get %s status", __FUNCTION__, __LINE__, supplicant_prop_name);

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(supplicant_prop_name);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                return 0;
            } else if (__system_property_serial(pi) != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                return -1;
            }
        }
#else
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (DBG)
                ALOGD("%s:%d get %s status %s", __FUNCTION__, __LINE__, supplicant_prop_name, supp_status);

            if (strcmp(supp_status, "running") == 0)
                return 0;
        }
#endif
        usleep(100000);
    }
    return -1;
}
#endif
// Engle add for MTK, end

// Engle add for MTK, start
#ifdef TARGET_MTK

int wifi_stop_supplicant(int p2p_supported)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */
    void *handle = dlopen("/system/lib/libpalwlan_mtk.so", RTLD_LAZY);
    int ret = 0;
    typedef void (*pal_set_wlan_down_t)();
    typedef void (*pal_send_wlan_off_event_t)();
    if (p2p_supported && strcmp(supplicant_name, "") == 0) {
        strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
        strcpy(supplicant_prop_name, P2P_PROP_NAME);
    }

    if (DBG)
        ALOGD("%s:%d enter, Stop \"%s\" [%d]", __FUNCTION__, __LINE__, supplicant_name, p2p_supported);

    /* Check whether supplicant already stopped */
    if (property_get(supplicant_prop_name, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        ret = 0;
        goto out;
    }

    property_set("ctl.stop", supplicant_name);
    sched_yield();

    while (count-- > 0) {
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0) {
                  ret = 0;
                  goto out;
            }
        }
        usleep(100000);
    }
    ALOGE("Failed to stop supplicant");
    ret = -1;
out:
   if (handle != NULL) {
       if (0 == ret) {
            pal_set_wlan_down_t pal_set_wlan_down = (pal_set_wlan_down_t)dlsym(handle, "pal_set_wlan_down");
            if (NULL == pal_set_wlan_down)
                ALOGE("Map pal_set_wlan_down error (%s)", dlerror());
            else
                  pal_set_wlan_down();
            pal_send_wlan_off_event_t pal_send_wlan_off_event = (pal_send_wlan_off_event_t)dlsym(handle, "pal_send_wlan_off_event");
            if (NULL == pal_send_wlan_off_event)
                ALOGE("Map pal_send_wlan_off_event error (%s)", dlerror());
            else
                pal_send_wlan_off_event();
            if (DBG && pal_set_wlan_down != NULL && pal_send_wlan_off_event != NULL)
                ALOGD("[PAL] wifi_stop_supplicant pass\n");
        }
        dlclose(handle);
        
    }
    return ret;
}

#else

int wifi_stop_supplicant(int p2p_supported)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    if (p2p_supported) {
        strcpy(supplicant_name, P2P_SUPPLICANT_NAME);
        strcpy(supplicant_prop_name, P2P_PROP_NAME);
    } else {
        strcpy(supplicant_name, SUPPLICANT_NAME);
        strcpy(supplicant_prop_name, SUPP_PROP_NAME);
    }

    /* Check whether supplicant already stopped */
    if (property_get(supplicant_prop_name, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

#ifdef USES_TI_MAC80211
    if (p2p_supported && add_remove_p2p_interface(0) < 0) {
        ALOGE("Wi-Fi - could not remove p2p interface");
        return -1;
    }
#endif

    property_set("ctl.stop", supplicant_name);
    sched_yield();

    while (count-- > 0) {
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    ALOGE("Failed to stop supplicant");
    return -1;
}

#endif

int wifi_connect_on_socket_path(const char *path)
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

    if (DBG)
        ALOGD("%s:%d path %s", __FUNCTION__, __LINE__,  path);

    /* Make sure supplicant is running */
    if (!property_get(supplicant_prop_name, supp_status, NULL)
            || strcmp(supp_status, "running") != 0) {
        ALOGE("Supplicant not running, cannot connect");
        return -1;
    }

    ctrl_conn = wpa_ctrl_open(path);
    if (ctrl_conn == NULL) {
        ALOGE("Unable to open connection to supplicant on \"%s\": %s",
             path, strerror(errno));
        return -1;
    }
    monitor_conn = wpa_ctrl_open(path);
    if (monitor_conn == NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
        return -1;
    }
    if (wpa_ctrl_attach(monitor_conn) != 0) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, exit_sockets) == -1) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }

    return 0;
}

/* Establishes the control and monitor socket connections on the interface */
int wifi_connect_to_supplicant()
{
    static char path[PATH_MAX];

    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);

    if (access(IFACE_DIR, F_OK) == 0) {
        snprintf(path, sizeof(path), "%s/%s", IFACE_DIR, primary_iface);
    } else {
        snprintf(path, sizeof(path), "@android:wpa_%s", primary_iface);
    }

    if (DBG)
        ALOGD("Enter wifi_connect_to_supplicant %s", path);

    return wifi_connect_on_socket_path(path);
}

int wifi_send_command(const char *cmd, char *reply, size_t *reply_len)
{
    int ret;

    if (DBG)
        ALOGD("%s:%d enter cmd %s", __FUNCTION__, __LINE__, cmd);

    if (ctrl_conn == NULL) {
        ALOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
        return -1;
    }
    ret = wpa_ctrl_request(ctrl_conn, cmd, strlen(cmd), reply, reply_len, NULL);
    if (ret == -2) {
        ALOGD("'%s' command timed out.\n", cmd);
        /* unblocks the monitor receive socket for termination */
        TEMP_FAILURE_RETRY(write(exit_sockets[0], "T", 1));
        return -2;
    } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    if (strncmp(cmd, "PING", 4) == 0) {
        reply[*reply_len] = '\0';
    }
    return 0;
}

int wifi_ctrl_recv(char *reply, size_t *reply_len)
{
    int res;
    int ctrlfd = wpa_ctrl_get_fd(monitor_conn);
    struct pollfd rfds[2];
    memset(rfds, 0, 2 * sizeof(struct pollfd));
    rfds[0].fd = ctrlfd;
    rfds[0].events |= POLLIN;
    rfds[1].fd = exit_sockets[1];
    rfds[1].events |= POLLIN;
    res = TEMP_FAILURE_RETRY(poll(rfds, 2, -1));
    if (res < 0) {
        ALOGE("Error poll = %d", res);
        return res;
    }
    if (rfds[0].revents & POLLIN) {
        int ret = wpa_ctrl_recv(monitor_conn, reply, reply_len);
        if (DBG)
            ALOGD("%s:%d enter reply %s", __FUNCTION__, __LINE__, reply);
        return ret;
    }

    /* it is not rfds[0], then it must be rfts[1] (i.e. the exit socket)
     * or we timed out. In either case, this call has failed ..
     */
    return -2;
}

int wifi_wait_on_socket(char *buf, size_t buflen)
{
    size_t nread = buflen - 1;
    int result;
    // Engle, Add IFNAME=prefix
    char suffixBuf[buflen];
    char *match, *match2;

    if (monitor_conn == NULL) {
        return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - connection closed");
    }

    result = wifi_ctrl_recv(buf, &nread);

    /* Terminate reception on exit socket */
    if (result == -2) {
        return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - connection closed");
    }

    // if (DBG)
    //    ALOGD("%s:%d wifi_ctrl_recv return %d", __FUNCTION__, __LINE__, result);
    if (result < 0) {
        ALOGD("wifi_ctrl_recv failed: %s\n", strerror(errno));
        return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - recv error");
    }
    buf[nread] = '\0';
    /* Check for EOF on the socket */
    if (result == 0 && nread == 0) {
        /* Fabricate an event to pass up */
        ALOGD("Received EOF on supplicant socket\n");
        return snprintf(buf, buflen, WPA_EVENT_TERMINATING " - signal 0 received");
    }
    /*
     * Events strings are in the format
     *
     *     IFNAME=iface <N>CTRL-EVENT-XXX 
     *        or
     *     <N>CTRL-EVENT-XXX 
     *
     * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
     * etc.) and XXX is the event name. The level information is not useful
     * to us, so strip it off.
     */
    // Engle, add IFNAME=prefix, start
    memcpy(suffixBuf, buf, buflen);
    memset(buf, 0, buflen);
    // if (DBG)
    //     ALOGD("%s:%d receive from (%s) is %s", __FUNCTION__, __LINE__, primary_iface, suffixBuf);
    snprintf(buf, buflen, "%s%s %s", IFNAME, primary_iface, suffixBuf);
    nread = nread + IFNAMELEN + strlen(primary_iface) + 1;
    // Engle, add IFNAME=prefix, end
    if (strncmp(buf, IFNAME, IFNAMELEN) == 0) {
        match = strchr(buf, ' ');
        if (match != NULL) {
            if (match[1] == '<') {
                match2 = strchr(match + 2, '>');
                if (match2 != NULL) {
                    nread -= (match2 - match);
                    memmove(match + 1, match2 + 1, nread - (match - buf) + 1);
                }
            }
        } else {
            return snprintf(buf, buflen, "%s", WPA_EVENT_IGNORE);
        }
    } else if (buf[0] == '<') {
        match = strchr(buf, '>');
        if (match != NULL) {
            nread -= (match + 1 - buf);
            memmove(buf, match + 1, nread + 1);
            ALOGV("supplicant generated event without interface - %s\n", buf);
        }
    } else {
        /* let the event go as is! */
        ALOGW("supplicant generated event without interface and without message level - %s\n", buf);
    }

    return nread;
}

int wifi_wait_for_event(char *buf, size_t buflen)
{
    // Engle add for event log
    // return wifi_wait_on_socket(buf, buflen);
    int ret = wifi_wait_on_socket(buf, buflen);
    if (DBG)
        ALOGD("[%s]GET \"%s\"", primary_iface, buf);
    return ret;
}

void wifi_close_sockets()
{
    if (DBG)
        ALOGD("[%s] wifi_close_sockets", primary_iface);

    if (ctrl_conn != NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
    }

    if (monitor_conn != NULL) {
        wpa_ctrl_close(monitor_conn);
        monitor_conn = NULL;
    }

    if (exit_sockets[0] >= 0) {
        close(exit_sockets[0]);
        exit_sockets[0] = -1;
    }

    if (exit_sockets[1] >= 0) {
        close(exit_sockets[1]);
        exit_sockets[1] = -1;
    }
}

void wifi_close_supplicant_connection()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds to ensure init has stopped stupplicant */

    ALOGD("Enter wifi_close_supplicant_connection\n");

    wifi_close_sockets();

    while (count-- > 0) {
        if (property_get(supplicant_prop_name, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return;
        }
        usleep(100000);
    }
}

int wifi_command(const char *command, char *reply, size_t *reply_len)
{
    // Engle, remove IFNAME= for old wifi driver, start
    char *match;
    int nCmdLengh = strlen(command);
    char buf[nCmdLengh];
    int ret = -1;
    memset(buf, 0, nCmdLengh);
    if (strncmp(command, IFNAME, IFNAMELEN) == 0) {
        match = strchr(command, ' ');
        if (match != NULL) {
            match++;
            memcpy((void *)&buf, match, strlen(match));
            ret = wifi_send_command((const char*)&buf, reply, reply_len);
        } 
    } else {
        ret = wifi_send_command(command, reply, reply_len);
    }
    if (DBG)
        ALOGD("[%s] Set - %s\n And reply %s\n", primary_iface, command, reply);
    return ret;
}

const char *wifi_get_fw_path(int fw_type)
{
    if (DBG > 1)
        ALOGD("wifi_get_fw_path - %d\n", fw_type);
    switch (fw_type) {
        case WIFI_GET_FW_PATH_STA:
            return WIFI_DRIVER_FW_PATH_STA;
        case WIFI_GET_FW_PATH_AP:
            return WIFI_DRIVER_FW_PATH_AP;
        case WIFI_GET_FW_PATH_P2P:
            return WIFI_DRIVER_FW_PATH_P2P;
    }
    return NULL;
}

// Engle, add for MTK, start
#ifndef TARGET_MTK
int wifi_change_fw_path(const char *fwpath)
{
    int len;
    int fd;
    int ret = 0;

    if (!fwpath)
        return ret;
    fd = TEMP_FAILURE_RETRY(open(WIFI_DRIVER_FW_PATH_PARAM, O_WRONLY));
    if (fd < 0) {
        ALOGE("Failed to open wlan fw path param (%s)", strerror(errno));
        return -1;
    }
    len = strlen(fwpath) + 1;
    if (TEMP_FAILURE_RETRY(write(fd, fwpath, len)) != len) {
        ALOGE("Failed to write wlan fw path param (%s)", strerror(errno));
        ret = -1;
    }
    close(fd);
    return ret;
}
#else
int wifi_change_fw_path(const char* fwpath) {
    if (DBG)
        ALOGD("wifi_change_fw_path [%s]", fwpath);
    if (strcmp(WIFI_DRIVER_FW_PATH_STA, fwpath) == 0) {
          wifi_set_p2p_mod(1, 0);
          return 0;
    } else if (strcmp(WIFI_DRIVER_FW_PATH_AP, fwpath) == 0) {
        wifi_set_p2p_mod(1, 1);
        return 0;
    } else if (strcmp(WIFI_DRIVER_FW_PATH_P2P, fwpath) == 0) {
          wifi_set_p2p_mod(1, 0);
          return 0;
    }
    ALOGE("Failed to write wlan fw path, unknow path");
    return -1;
}
#endif
// Engle, add for MTK, end

int wifi_set_mode(int mode) {
    if (DBG)
        ALOGD("%s:%d enter, set mode to %d", __FUNCTION__, __LINE__, mode);
    wifi_mode = mode;
    return 0;
}

// Engle, add for MTK, start
#ifdef TARGET_MTK
void wifi_set_power(int enable) {
    int fd;
    int len;
    const char buffer = (enable ? '1' : '0');
    if (DBG)
        ALOGD("%s:%d enter, set power to %d", __FUNCTION__, __LINE__, enable);

    fd = open(WIFI_POWER_PATH, O_WRONLY);
    if (fd < 0) {
        ALOGE("Open \"%s\" failed (%s)", WIFI_POWER_PATH, strerror(errno));
        goto out;
    }
    len = write(fd, &buffer, 1);
    if (len < 0) {
        ALOGE("Set \"%s\" [%c] failed (%s)", WIFI_POWER_PATH, buffer, strerror(errno));
        goto out;
    }
out:
    if (fd > 0) {
        close(fd);
    }
    halDoCommand(enable ? "load wifi" : "unload wifi");
}

void halDoCommand (const char* cmd) {
    char *final_cmd;
    int sock = socket_local_client("hald", ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
    if (sock < 0) {
        ALOGE("Error connecting (%s)", strerror(errno));
        return;
    }
    if (DBG)
        ALOGD("hal cmd %s", cmd);
    asprintf(&final_cmd, "%s %s", "hal", cmd);
    if (write(sock, final_cmd, strlen(final_cmd) + 1) <= 0) {
          ALOGE("Hal cmd error: (%s)", strerror(errno));
          free(final_cmd);
        close(sock);
    } else {
          free(final_cmd);
          halDoMonitor(sock);
    }
}

int halDoMonitor(int sock) {
    char *buffer = malloc(0x1000);
    fd_set read_fds;
    struct timeval to;
    int rc = 0;

    to.tv_sec = 10;
    to.tv_usec = 0;

    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    if ((rc = select(sock +1, &read_fds, NULL, NULL, &to)) <= 0) {
        int res = errno;
        ALOGE("Error in select (%s)\n", strerror(errno));
        free(buffer);
        close(sock);
        return res;
    } else {
        memset(buffer, 0, 0x1000);
        if ((rc = read(sock, buffer, 0x1000)) <= 0) {
            int res = errno;
            if (rc == 0)
                ALOGE("Lost connection to Hald - did it crash?\n");
            else
                ALOGE("Error reading data (%s)\n", strerror(errno));
            free(buffer);
            close(sock);
            if (rc == 0)
                return ECONNRESET;
            return res;
        }

        int offset = 0;
        int i = 0;

        for (i = 0; i < rc; i++) {
            if (buffer[i] == '\0') {
                int code;
                char tmp[4];

                strncpy(tmp, buffer + offset, 3);
                tmp[3] = '\0';
                code = atoi(tmp);
                if (DBG)
                    ALOGD("Hal cmd response: \"%s\"\n", buffer + offset);

                if (code >= 200 && code < 400) {
                    free(buffer);
                    close(sock);
                    return 0;
                }
                offset = i + 1;
            }
        }
    }
    free(buffer);
    close(sock);
    return 0;
}

void wifi_set_p2p_mod(int enableP2P, int enableAP) {
    if (DBG)
        ALOGD("%s:%d enter, enableP2P %d, enableAP %d", __FUNCTION__, __LINE__, enableP2P, enableAP);
    if (enableP2P != 0) {
        if (enableAP != 0) {
            halDoCommand("load hostspot");
        } else {
            halDoCommand("load p2p");
        }
    } else {
        halDoCommand("unload p2p");
        halDoCommand("unload hostspot");
    }
}

int wifi_ap_start_supplicant() {
    if (DBG)
        ALOGD("%s:%d enter", __FUNCTION__, __LINE__);
    return wifi_start_supplicant(WIFI_GET_FW_PATH_AP);
}
#endif
// Engle, add for MTK, end