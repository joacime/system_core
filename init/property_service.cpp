/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <sys/poll.h>

#include <memory>

#include <cutils/misc.h>
#include <cutils/sockets.h>
#include <cutils/multiuser.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include <selinux/android.h>
#include <selinux/selinux.h>
#include <selinux/label.h>

#include <fs_mgr.h>
#include <android-base/file.h>
#include "bootimg.h"

#include "property_service.h"
#include "init.h"
#include "util.h"
#include "log.h"

#define PERSISTENT_PROPERTY_DIR  "/data/property"
#define FSTAB_PREFIX "/fstab."
#define RECOVERY_MOUNT_POINT "/recovery"

static int persistent_properties_loaded = 0;

static int property_set_fd = -1;

void property_init() {
    if (__system_property_area_init()) {
        LOG(ERROR) << "Failed to initialize property area";
        exit(1);
    }
}

static int check_mac_perms(const char *name, char *sctx, struct ucred *cr)
{
    char *tctx = NULL;
    int result = 0;
    property_audit_data audit_data;

    if (!sctx)
        goto err;

    if (!sehandle_prop)
        goto err;

    if (selabel_lookup(sehandle_prop, &tctx, name, 1) != 0)
        goto err;

    audit_data.name = name;
    audit_data.cr = cr;

    if (selinux_check_access(sctx, tctx, "property_service", "set", reinterpret_cast<void*>(&audit_data)) == 0)
        result = 1;

    freecon(tctx);
 err:
    return result;
}

static int check_control_mac_perms(const char *name, char *sctx, struct ucred *cr)
{
    /*
     *  Create a name prefix out of ctl.<service name>
     *  The new prefix allows the use of the existing
     *  property service backend labeling while avoiding
     *  mislabels based on true property prefixes.
     */
    char ctl_name[PROP_VALUE_MAX+4];
    int ret = snprintf(ctl_name, sizeof(ctl_name), "ctl.%s", name);

    if (ret < 0 || (size_t) ret >= sizeof(ctl_name))
        return 0;

    return check_mac_perms(ctl_name, sctx, cr);
}

std::string property_get(const char* name) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(name, value);
    return value;
}

static void write_persistent_property(const char *name, const char *value)
{
    char tempPath[PATH_MAX];
    char path[PATH_MAX];
    int fd;

    snprintf(tempPath, sizeof(tempPath), "%s/.temp.XXXXXX", PERSISTENT_PROPERTY_DIR);
    fd = mkstemp(tempPath);
    if (fd < 0) {
        PLOG(ERROR) << "Unable to write persistent property to temp file " << tempPath;
        return;
    }
    write(fd, value, strlen(value));
    fsync(fd);
    close(fd);

    snprintf(path, sizeof(path), "%s/%s", PERSISTENT_PROPERTY_DIR, name);
    if (rename(tempPath, path)) {
        PLOG(ERROR) << "Unable to rename persistent property file " << tempPath << " to " << path;
        unlink(tempPath);
    }
}

bool is_legal_property_name(const std::string &name)
{
    size_t namelen = name.size();

    if (namelen >= PROP_NAME_MAX) return false;
    if (namelen < 1) return false;
    if (name[0] == '.') return false;
    if (name[namelen - 1] == '.') return false;

    /* Only allow alphanumeric, plus '.', '-', '@', or '_' */
    /* Don't allow ".." to appear in a property name */
    for (size_t i = 0; i < namelen; i++) {
        if (name[i] == '.') {
            // i=0 is guaranteed to never have a dot. See above.
            if (name[i-1] == '.') return false;
            continue;
        }
        if (name[i] == '_' || name[i] == '-' || name[i] == '@') continue;
        if (name[i] >= 'a' && name[i] <= 'z') continue;
        if (name[i] >= 'A' && name[i] <= 'Z') continue;
        if (name[i] >= '0' && name[i] <= '9') continue;
        return false;
    }

    return true;
}

int property_set(const char* name, const char* value) {
    size_t valuelen = strlen(value);

    if (!is_legal_property_name(name)) {
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: bad name";
        return -1;
    }
    if (valuelen >= PROP_VALUE_MAX) {
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                   << "value too long";
        return -1;
    }

    if (strcmp("selinux.restorecon_recursive", name) == 0 && valuelen > 0) {
        if (restorecon(value, SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            LOG(ERROR) << "Failed to restorecon_recursive " << value;
        }
    }

    prop_info* pi = (prop_info*) __system_property_find(name);
    if (pi != nullptr) {
        // ro.* properties are actually "write-once".
        if (!strncmp(name, "ro.", 3)) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "property already set";
            return -1;
        }

        __system_property_update(pi, value, valuelen);
    } else {
        int rc = __system_property_add(name, strlen(name), value, valuelen);
        if (rc < 0) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "__system_property_add failed";
            return rc;
        }
    }

    // Don't write properties to disk until after we have read all default
    // properties to prevent them from being overwritten by default values.
    if (persistent_properties_loaded && strncmp("persist.", name, strlen("persist.")) == 0) {
        write_persistent_property(name, value);
    }
    property_changed(name, value);
    return 0;
}

static void handle_property_set_fd()
{
    prop_msg msg;
    int r;
    char * source_ctx = NULL;

    int s = accept(property_set_fd, nullptr, nullptr);
    if (s == -1) {
        return;
    }

    /* Check socket options here */
    struct ucred cr;
    socklen_t cr_size = sizeof(cr);
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cr_size) < 0) {
        close(s);
        PLOG(ERROR) << "Unable to receive socket options";
        return;
    }

    static constexpr int timeout_ms = 2 * 1000;  /* Default 2 sec timeout for caller to send property. */
    struct pollfd ufds[1];
    ufds[0].fd = s;
    ufds[0].events = POLLIN;
    ufds[0].revents = 0;
    int nr = TEMP_FAILURE_RETRY(poll(ufds, 1, timeout_ms));
    if (nr == 0) {
        LOG(ERROR) << "sys_prop: timeout waiting for uid " << cr.uid << " to send property message.";
        close(s);
        return;
    } else if (nr < 0) {
        PLOG(ERROR) << "sys_prop: error waiting for uid " << cr.uid << " to send property message";
        close(s);
        return;
    }

    r = TEMP_FAILURE_RETRY(recv(s, &msg, sizeof(msg), MSG_DONTWAIT));
    if(r != sizeof(prop_msg)) {
        PLOG(ERROR) << "sys_prop: mis-match msg size received: " << r << " expected: " << sizeof(prop_msg);
        close(s);
        return;
    }

    switch(msg.cmd) {
    case PROP_MSG_SETPROP:
        msg.name[PROP_NAME_MAX-1] = 0;
        msg.value[PROP_VALUE_MAX-1] = 0;

        if (!is_legal_property_name(msg.name)) {
            LOG(ERROR) << "sys_prop: illegal property name \"" << msg.name << "\"";
            close(s);
            return;
        }

        getpeercon(s, &source_ctx);

        if(memcmp(msg.name,"ctl.",4) == 0) {
            // Keep the old close-socket-early behavior when handling
            // ctl.* properties.
            close(s);
            if (check_control_mac_perms(msg.value, source_ctx, &cr)) {
                handle_control_message((char*) msg.name + 4, (char*) msg.value);
            } else {
                LOG(ERROR) << "sys_prop: Unable to " << (msg.name + 4)
                           << " service ctl [" << msg.value << "]"
                           << " uid:" << cr.uid
                           << " gid:" << cr.gid
                           << " pid:" << cr.pid;
            }
        } else {
            if (check_mac_perms(msg.name, source_ctx, &cr)) {
                property_set((char*) msg.name, (char*) msg.value);
            } else {
                LOG(ERROR) << "sys_prop: permission denied uid:" << cr.uid << " name:" << msg.name;
            }

            // Note: bionic's property client code assumes that the
            // property server will not close the socket until *AFTER*
            // the property is written to memory.
            close(s);
        }
        freecon(source_ctx);
        break;

    default:
        close(s);
        break;
    }
}

static void load_properties_from_file(const char *, const char *);

/*
 * Filter is used to decide which properties to load: NULL loads all keys,
 * "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
 */
static void load_properties(char *data, const char *filter)
{
    char *key, *value, *eol, *sol, *tmp, *fn;
    size_t flen = 0;

    if (filter) {
        flen = strlen(filter);
    }

    sol = data;
    while ((eol = strchr(sol, '\n'))) {
        key = sol;
        *eol++ = 0;
        sol = eol;

        while (isspace(*key)) key++;
        if (*key == '#') continue;

        tmp = eol - 2;
        while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

        if (!strncmp(key, "import ", 7) && flen == 0) {
            fn = key + 7;
            while (isspace(*fn)) fn++;

            key = strchr(fn, ' ');
            if (key) {
                *key++ = 0;
                while (isspace(*key)) key++;
            }

            load_properties_from_file(fn, key);

        } else {
            value = strchr(key, '=');
            if (!value) continue;
            *value++ = 0;

            tmp = value - 2;
            while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

            while (isspace(*value)) value++;

            if (flen > 0) {
                if (filter[flen - 1] == '*') {
                    if (strncmp(key, filter, flen - 1)) continue;
                } else {
                    if (strcmp(key, filter)) continue;
                }
            }

            property_set(key, value);
        }
    }
}

// Filter is used to decide which properties to load: NULL loads all keys,
// "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
static void load_properties_from_file(const char* filename, const char* filter) {
    Timer t;
    std::string data;
    if (!read_file(filename, &data)) {
        PLOG(WARNING) << "Couldn't load properties from " << filename;
        return;
    }
    data.push_back('\n');
    load_properties(&data[0], filter);
    LOG(VERBOSE) << "(Loading properties from " << filename << " took " << t << ".)";
}

static void load_persistent_properties() {
    persistent_properties_loaded = 1;

    std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir(PERSISTENT_PROPERTY_DIR), closedir);
    if (!dir) {
        PLOG(ERROR) << "Unable to open persistent property directory \""
                    << PERSISTENT_PROPERTY_DIR << "\"";
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != NULL) {
        if (strncmp("persist.", entry->d_name, strlen("persist."))) {
            continue;
        }
        if (entry->d_type != DT_REG) {
            continue;
        }

        // Open the file and read the property value.
        int fd = openat(dirfd(dir.get()), entry->d_name, O_RDONLY | O_NOFOLLOW);
        if (fd == -1) {
            PLOG(ERROR) << "Unable to open persistent property file \"" << entry->d_name << "\"";
            continue;
        }

        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            PLOG(ERROR) << "fstat on property file \"" << entry->d_name << "\" failed";
            close(fd);
            continue;
        }

        // File must not be accessible to others, be owned by root/root, and
        // not be a hard link to any other file.
        if (((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) || sb.st_uid != 0 || sb.st_gid != 0 || sb.st_nlink != 1) {
            PLOG(ERROR) << "skipping insecure property file " << entry->d_name
                        << " (uid=" << sb.st_uid << " gid=" << sb.st_gid
                        << " nlink=" << sb.st_nlink << " mode=" << std::oct << sb.st_mode << ")";
            close(fd);
            continue;
        }

        char value[PROP_VALUE_MAX];
        int length = read(fd, value, sizeof(value) - 1);
        if (length >= 0) {
            value[length] = 0;
            property_set(entry->d_name, value);
        } else {
            PLOG(ERROR) << "Unable to read persistent property file " << entry->d_name;
        }
        close(fd);
    }
}

void property_load_boot_defaults() {
    load_properties_from_file(PROP_PATH_RAMDISK_DEFAULT, NULL);
    load_properties_from_file(PROP_PATH_ODM_DEFAULT, NULL);
    load_properties_from_file(PROP_PATH_VENDOR_DEFAULT, NULL);
}

static void load_override_properties() {
    if (ALLOW_LOCAL_PROP_OVERRIDE) {
        std::string debuggable = property_get("ro.debuggable");
        if (debuggable == "1") {
            load_properties_from_file(PROP_PATH_LOCAL_OVERRIDE, NULL);
        }
    }
}

/* When booting an encrypted system, /data is not mounted when the
 * property service is started, so any properties stored there are
 * not loaded.  Vold triggers init to load these properties once it
 * has mounted /data.
 */
void load_persist_props(void) {
    load_override_properties();
    /* Read persistent properties after all default values have been loaded. */
    load_persistent_properties();
}

void load_recovery_id_prop() {
    std::string ro_hardware = property_get("ro.hardware");
    if (ro_hardware.empty()) {
        LOG(ERROR) << "ro.hardware not set - unable to load recovery id";
        return;
    }
    std::string fstab_filename = FSTAB_PREFIX + ro_hardware;

    std::unique_ptr<fstab, void(*)(fstab*)> tab(fs_mgr_read_fstab(fstab_filename.c_str()),
                                                fs_mgr_free_fstab);
    if (!tab) {
        PLOG(ERROR) << "unable to read fstab " << fstab_filename;
        return;
    }

    fstab_rec* rec = fs_mgr_get_entry_for_mount_point(tab.get(), RECOVERY_MOUNT_POINT);
    if (rec == NULL) {
        LOG(ERROR) << "/recovery not specified in fstab";
        return;
    }

    int fd = open(rec->blk_device, O_RDONLY);
    if (fd == -1) {
        PLOG(ERROR) << "error opening block device " << rec->blk_device;
        return;
    }

    boot_img_hdr hdr;
    if (android::base::ReadFully(fd, &hdr, sizeof(hdr))) {
        std::string hex = bytes_to_hex(reinterpret_cast<uint8_t*>(hdr.id), sizeof(hdr.id));
        property_set("ro.recovery_id", hex.c_str());
    } else {
        PLOG(ERROR) << "error reading /recovery";
    }

    close(fd);
}

void load_system_props() {
    load_properties_from_file(PROP_PATH_SYSTEM_BUILD, NULL);
    load_properties_from_file(PROP_PATH_ODM_BUILD, NULL);
    load_properties_from_file(PROP_PATH_VENDOR_BUILD, NULL);
    load_properties_from_file(PROP_PATH_FACTORY, "ro.*");
    load_recovery_id_prop();
}

void start_property_service() {
    property_set_fd = create_socket(PROP_SERVICE_NAME, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0666, 0, 0, NULL);
    if (property_set_fd == -1) {
        PLOG(ERROR) << "start_property_service socket creation failed";
        exit(1);
    }

    listen(property_set_fd, 8);

    register_epoll_handler(property_set_fd, handle_property_set_fd);
}
