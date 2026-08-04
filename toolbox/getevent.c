#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <limits.h>
#include <time.h>
#include <sys/poll.h>
#include <linux/input.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>

struct label {
    const char *name;
    int value;
};

#define LABEL(constant) { #constant, constant }
#define LABEL_END { NULL, -1 }

static struct label key_value_labels[] = {
        { "UP", 0 },
        { "DOWN", 1 },
        { "REPEAT", 2 },
        LABEL_END,
};

#include "input.h-labels.h"

#undef LABEL
#undef LABEL_END

#define BST_EV_ABS_AND_KEY		0x17

static struct pollfd *ufds;
static char **device_names;
static int nfds;

// Track mouse button state for multiple Mouse Devices
struct mouse_device_state {
    int device_index;
    int buttons_pressed;
    int last_x;
    int last_y;
    int skip_sync_event;
};

static struct mouse_device_state *mouse_devices = NULL;
static int mouse_device_count = 0;

// Event filter variables
static int event_filter_enabled = 0;
static uint32_t event_filter_mask = 0;
static char **device_filter_paths = NULL;
static int device_filter_count = 0;

enum {
    PRINT_DEVICE_ERRORS     = 1U << 0,
    PRINT_DEVICE            = 1U << 1,
    PRINT_DEVICE_NAME       = 1U << 2,
    PRINT_DEVICE_INFO       = 1U << 3,
    PRINT_VERSION           = 1U << 4,
    PRINT_POSSIBLE_EVENTS   = 1U << 5,
    PRINT_INPUT_PROPS       = 1U << 6,
    PRINT_HID_DESCRIPTOR    = 1U << 7,

    PRINT_ALL_INFO          = (1U << 8) - 1,

    PRINT_LABELS            = 1U << 16,
};

static const char *get_label(const struct label *labels, int value)
{
    while(labels->name && value != labels->value) {
        labels++;
    }
    return labels->name;
}

static int device_in_filter(const char *device)
{
    if (device_filter_count == 0) return 1; // no filter means allow all
    for (int i = 0; i < device_filter_count; i++) {
        if (strcmp(device_filter_paths[i], device) == 0)
            return 1;
    }
    return 0;
}

// Find mouse device state by device index
static struct mouse_device_state *find_mouse_device(int device_index)
{
    for (int i = 0; i < mouse_device_count; i++) {
        if (mouse_devices[i].device_index == device_index) {
            return &mouse_devices[i];
        }
    }
    return NULL;
}

// Add a mouse device to tracking
static void add_mouse_device(int device_index)
{
    struct mouse_device_state *new_devices = realloc(mouse_devices,
        sizeof(struct mouse_device_state) * (mouse_device_count + 1));
    if (new_devices == NULL) {
        fprintf(stderr, "out of memory for mouse device tracking\n");
        return;
    }
    mouse_devices = new_devices;
    mouse_devices[mouse_device_count].device_index = device_index;
    mouse_devices[mouse_device_count].buttons_pressed = 0;
    mouse_devices[mouse_device_count].last_x = -1;
    mouse_devices[mouse_device_count].last_y = -1;
    mouse_devices[mouse_device_count].skip_sync_event = 0;
    mouse_device_count++;
}

// Remove a mouse device from tracking
static void remove_mouse_device(int device_index)
{
    for (int i = 0; i < mouse_device_count; i++) {
        if (mouse_devices[i].device_index == device_index) {
            // Shift remaining devices
            memmove(&mouse_devices[i], &mouse_devices[i + 1],
                sizeof(struct mouse_device_state) * (mouse_device_count - i - 1));
            mouse_device_count--;
            struct mouse_device_state *new_devices = realloc(mouse_devices,
                sizeof(struct mouse_device_state) * mouse_device_count);
            if (mouse_device_count > 0 && new_devices != NULL) {
                mouse_devices = new_devices;
            } else if (mouse_device_count == 0) {
                free(mouse_devices);
                mouse_devices = NULL;
            }
            return;
        }
    }
}

// Adjust mouse device indices when a device is removed before them
static void adjust_mouse_device_indices(int removed_index)
{
    for (int i = 0; i < mouse_device_count; i++) {
        if (mouse_devices[i].device_index > removed_index) {
            mouse_devices[i].device_index--;
        }
    }
}


static int parse_event_types(const char *arg)
{
    char *token, *str, *saveptr;
    uint32_t mask = 0;

    str = strdup(arg);
    if (!str) return -1;

    token = strtok_r(str, ",", &saveptr);
    while (token != NULL) {
        if (strcmp(token, "BST_EV_ABS_AND_KEY") == 0) {
            mask |= (1 << BST_EV_ABS_AND_KEY);
            mask |= (1 << EV_ABS);
            mask |= (1 << EV_KEY);
        }
        else if (strcmp(token, "EV_SYN") == 0) {
            mask |= (1 << EV_SYN);
        } else if (strcmp(token, "EV_KEY") == 0) {
            mask |= (1 << EV_KEY);
        } else if (strcmp(token, "EV_REL") == 0) {
            mask |= (1 << EV_REL);
        } else if (strcmp(token, "EV_ABS") == 0) {
            mask |= (1 << EV_ABS);
        } else if (strcmp(token, "EV_MSC") == 0) {
            mask |= (1 << EV_MSC);
        } else if (strcmp(token, "EV_SW") == 0) {
            mask |= (1 << EV_SW);
        } else if (strcmp(token, "EV_LED") == 0) {
            mask |= (1 << EV_LED);
        } else if (strcmp(token, "EV_SND") == 0) {
            mask |= (1 << EV_SND);
        } else if (strcmp(token, "EV_REP") == 0) {
            mask |= (1 << EV_REP);
        } else if (strcmp(token, "EV_FF") == 0) {
            mask |= (1 << EV_FF);
        } else if (strcmp(token, "EV_PWR") == 0) {
            mask |= (1 << EV_PWR);
        } else if (strcmp(token, "EV_FF_STATUS") == 0) {
            mask |= (1 << EV_FF_STATUS);
        } else {
            fprintf(stderr, "Unknown event type: %s\n", token);
            free(str);
            return -1;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(str);
    event_filter_mask = mask;
    event_filter_enabled = 1;
    return 0;
}

static void print_startup_timestamp(void)
{
    struct timespec mono, real;

    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);

    double mono_sec = mono.tv_sec + mono.tv_nsec / 1e9;
    double real_sec = real.tv_sec + real.tv_nsec / 1e9;

    printf("[ %10.6f] epoch_time: %.6f\n", mono_sec, real_sec);
}


static int print_input_props(int fd)
{
    uint8_t bits[INPUT_PROP_CNT / 8];
    int i, j;
    int res;
    int count;
    const char *bit_label;

    printf("  input props:\n");
    res = ioctl(fd, EVIOCGPROP(sizeof(bits)), bits);
    if(res < 0) {
        printf("    <not available\n");
        return 1;
    }
    count = 0;
    for(i = 0; i < res; i++) {
        for(j = 0; j < 8; j++) {
            if (bits[i] & 1 << j) {
                bit_label = get_label(input_prop_labels, i * 8 + j);
                if(bit_label)
                    printf("    %s\n", bit_label);
                else
                    printf("    %04x\n", i * 8 + j);
                count++;
            }
        }
    }
    if (!count)
        printf("    <none>\n");
    return 0;
}

static int print_possible_events(int fd, int print_flags)
{
    uint8_t *bits = NULL;
    ssize_t bits_size = 0;
    const char* label;
    int i, j, k;
    int res, res2;
    struct label* bit_labels;
    const char *bit_label;

    printf("  events:\n");
    for(i = EV_KEY; i <= EV_MAX; i++) { // skip EV_SYN since we cannot query its available codes
        int count = 0;
        while(1) {
            res = ioctl(fd, EVIOCGBIT(i, bits_size), bits);
            if(res < bits_size)
                break;
            bits_size = res + 16;
            bits = realloc(bits, bits_size * 2);
            if (bits == NULL) err(1, "failed to allocate buffer of size %zd", bits_size);
        }
        res2 = 0;
        switch(i) {
            case EV_KEY:
                res2 = ioctl(fd, EVIOCGKEY(res), bits + bits_size);
                label = "KEY";
                bit_labels = key_labels;
                break;
            case EV_REL:
                label = "REL";
                bit_labels = rel_labels;
                break;
            case EV_ABS:
                label = "ABS";
                bit_labels = abs_labels;
                break;
            case EV_MSC:
                label = "MSC";
                bit_labels = msc_labels;
                break;
            case EV_LED:
                res2 = ioctl(fd, EVIOCGLED(res), bits + bits_size);
                label = "LED";
                bit_labels = led_labels;
                break;
            case EV_SND:
                res2 = ioctl(fd, EVIOCGSND(res), bits + bits_size);
                label = "SND";
                bit_labels = snd_labels;
                break;
            case EV_SW:
                res2 = ioctl(fd, EVIOCGSW(bits_size), bits + bits_size);
                label = "SW ";
                bit_labels = sw_labels;
                break;
            case EV_REP:
                label = "REP";
                bit_labels = rep_labels;
                break;
            case EV_FF:
                label = "FF ";
                bit_labels = ff_labels;
                break;
            case EV_PWR:
                label = "PWR";
                bit_labels = NULL;
                break;
            case EV_FF_STATUS:
                label = "FFS";
                bit_labels = ff_status_labels;
                break;
            default:
                res2 = 0;
                label = "???";
                bit_labels = NULL;
        }
        for(j = 0; j < res; j++) {
            for(k = 0; k < 8; k++)
                if(bits[j] & 1 << k) {
                    char down;
                    if(j < res2 && (bits[j + bits_size] & 1 << k))
                        down = '*';
                    else
                        down = ' ';
                    if(count == 0)
                        printf("    %s (%04x):", label, i);
                    else if((count & (print_flags & PRINT_LABELS ? 0x3 : 0x7)) == 0 || i == EV_ABS)
                        printf("\n               ");
                    if(bit_labels && (print_flags & PRINT_LABELS)) {
                        bit_label = get_label(bit_labels, j * 8 + k);
                        if(bit_label)
                            printf(" %.20s%c%*s", bit_label, down, (int) (20 - strlen(bit_label)), "");
                        else
                            printf(" %04x%c                ", j * 8 + k, down);
                    } else {
                        printf(" %04x%c", j * 8 + k, down);
                    }
                    if(i == EV_ABS) {
                        struct input_absinfo abs;
                        if(ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                            printf(" : value %d, min %d, max %d, fuzz %d, flat %d, resolution %d",
                                abs.value, abs.minimum, abs.maximum, abs.fuzz, abs.flat,
                                abs.resolution);
                        }
                    }
                    count++;
                }
        }
        if(count)
            printf("\n");
    }
    free(bits);
    return 0;
}

static void print_event(int type, int code, int value, int print_flags)
{
    const char *type_label, *code_label, *value_label;

    if (print_flags & PRINT_LABELS) {
        type_label = get_label(ev_labels, type);
        code_label = NULL;
        value_label = NULL;

        switch(type) {
            case EV_SYN:
                code_label = get_label(syn_labels, code);
                break;
            case EV_KEY:
                code_label = get_label(key_labels, code);
                value_label = get_label(key_value_labels, value);
                break;
            case EV_REL:
                code_label = get_label(rel_labels, code);
                break;
            case EV_ABS:
                code_label = get_label(abs_labels, code);
                switch(code) {
                    case ABS_MT_TOOL_TYPE:
                        value_label = get_label(mt_tool_labels, value);
                }
                break;
            case EV_MSC:
                code_label = get_label(msc_labels, code);
                break;
            case EV_LED:
                code_label = get_label(led_labels, code);
                break;
            case EV_SND:
                code_label = get_label(snd_labels, code);
                break;
            case EV_SW:
                code_label = get_label(sw_labels, code);
                break;
            case EV_REP:
                code_label = get_label(rep_labels, code);
                break;
            case EV_FF:
                code_label = get_label(ff_labels, code);
                break;
            case EV_FF_STATUS:
                code_label = get_label(ff_status_labels, code);
                break;
        }

        if (type_label)
            printf("%-12.12s", type_label);
        else
            printf("%04x        ", type);
        if (code_label)
            printf(" %-20.20s", code_label);
        else
            printf(" %04x                ", code);
        if (value_label)
            printf(" %-20.20s", value_label);
        else
            printf(" %08x            ", value);
    } else {
        printf("%04x %04x %08x", type, code, value);
    }
}

static void print_hid_descriptor(int bus, int vendor, int product)
{
    const char *dirname = "/sys/kernel/debug/hid";
    char prefix[16];
    DIR *dir;
    struct dirent *de;
    char filename[PATH_MAX];
    FILE *file;
    char line[2048];

    snprintf(prefix, sizeof(prefix), "%04X:%04X:%04X.", bus, vendor, product);

    dir = opendir(dirname);
    if(dir == NULL)
        return;
    while((de = readdir(dir))) {
        if (strstr(de->d_name, prefix) == de->d_name) {
            snprintf(filename, sizeof(filename), "%s/%s/rdesc", dirname, de->d_name);

            file = fopen(filename, "r");
            if (file) {
                printf("  HID descriptor: %s\n\n", de->d_name);
                while (fgets(line, sizeof(line), file)) {
                    fputs("    ", stdout);
                    fputs(line, stdout);
                }
                fclose(file);
                puts("");
            }
        }
    }
    closedir(dir);
}

static int open_device(const char *device, int print_flags)
{
    int version;
    int fd;
    int clkid = CLOCK_MONOTONIC;
    struct pollfd *new_ufds;
    char **new_device_names;
    char name[80];
    char location[80];
    char idstr[80];
    struct input_id id;

    fd = open(device, O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
        if(print_flags & PRINT_DEVICE_ERRORS)
            fprintf(stderr, "could not open %s, %s\n", device, strerror(errno));
        return -1;
    }
    
    if(ioctl(fd, EVIOCGVERSION, &version)) {
        if(print_flags & PRINT_DEVICE_ERRORS)
            fprintf(stderr, "could not get driver version for %s, %s\n", device, strerror(errno));
        return -1;
    }
    if(ioctl(fd, EVIOCGID, &id)) {
        if(print_flags & PRINT_DEVICE_ERRORS)
            fprintf(stderr, "could not get driver id for %s, %s\n", device, strerror(errno));
        return -1;
    }
    name[sizeof(name) - 1] = '\0';
    location[sizeof(location) - 1] = '\0';
    idstr[sizeof(idstr) - 1] = '\0';
    if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
        //fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
        name[0] = '\0';
    }
    if(ioctl(fd, EVIOCGPHYS(sizeof(location) - 1), &location) < 1) {
        //fprintf(stderr, "could not get location for %s, %s\n", device, strerror(errno));
        location[0] = '\0';
    }
    if(ioctl(fd, EVIOCGUNIQ(sizeof(idstr) - 1), &idstr) < 1) {
        //fprintf(stderr, "could not get idstring for %s, %s\n", device, strerror(errno));
        idstr[0] = '\0';
    }

    if (ioctl(fd, EVIOCSCLOCKID, &clkid) != 0) {
        fprintf(stderr, "Can't enable monotonic clock reporting: %s\n", strerror(errno));
        // a non-fatal error
    }

    new_ufds = realloc(ufds, sizeof(ufds[0]) * (nfds + 1));
    if(new_ufds == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    ufds = new_ufds;
    new_device_names = realloc(device_names, sizeof(device_names[0]) * (nfds + 1));
    if(new_device_names == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    device_names = new_device_names;

    if(print_flags & PRINT_DEVICE)
        printf("add device %d: %s\n", nfds, device);
    if(print_flags & PRINT_DEVICE_INFO)
        printf("  bus:      %04x\n"
               "  vendor    %04x\n"
               "  product   %04x\n"
               "  version   %04x\n",
               id.bustype, id.vendor, id.product, id.version);
    if(print_flags & PRINT_DEVICE_NAME)
        printf("  name:     \"%s\"\n", name);
    if(print_flags & PRINT_DEVICE_INFO)
        printf("  location: \"%s\"\n"
               "  id:       \"%s\"\n", location, idstr);
    if(print_flags & PRINT_VERSION)
        printf("  version:  %d.%d.%d\n",
               version >> 16, (version >> 8) & 0xff, version & 0xff);

    ufds[nfds].fd = fd;
    ufds[nfds].events = POLLIN;
    device_names[nfds] = strdup(device);
    nfds++;

    // Check if this is a Mouse Device (only if event filtering is enabled)
    if(event_filter_enabled && strcasecmp(name, "mouse device")) {
        add_mouse_device(nfds - 1); // nfds was just incremented, so use nfds - 1
    }

    if(print_flags & PRINT_POSSIBLE_EVENTS) {
        print_possible_events(fd, print_flags);
    }

    if(print_flags & PRINT_INPUT_PROPS) {
        print_input_props(fd);
    }
    if(print_flags & PRINT_HID_DESCRIPTOR) {
        print_hid_descriptor(id.bustype, id.vendor, id.product);
    }

    return 0;
}

int close_device(const char *device, int print_flags)
{
    int i;
    for(i = 1; i < nfds; i++) {
        if(strcmp(device_names[i], device) == 0) {
            int count = nfds - i - 1;
            if(print_flags & PRINT_DEVICE)
                printf("remove device %d: %s\n", i, device);

            // Remove mouse device tracking if this device is a mouse device
            if(find_mouse_device(i) != NULL) {
                remove_mouse_device(i);
            }
            // Adjust indices for mouse devices that come after this one
            adjust_mouse_device_indices(i);

            free(device_names[i]);
            memmove(device_names + i, device_names + i + 1, sizeof(device_names[0]) * count);
            memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
            nfds--;
            return 0;
        }
    }
    if(print_flags & PRINT_DEVICE_ERRORS)
        fprintf(stderr, "remote device: %s not found\n", device);
    return -1;
}

static int read_notify(const char *dirname, int nfd, int print_flags)
{
    int res;
    char devname[PATH_MAX];
    char *filename;
    char event_buf[512];
    int event_size;
    int event_pos = 0;
    struct inotify_event *event;

    res = read(nfd, event_buf, sizeof(event_buf));
    if(res < (int)sizeof(*event)) {
        if(errno == EINTR)
            return 0;
        fprintf(stderr, "could not get inotify events, %s\n", strerror(errno));
        return 1;
    }
    //printf("got %d bytes of event information\n", res);

    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';

    while(res >= (int)sizeof(*event)) {
        event = (struct inotify_event *)(event_buf + event_pos);
        //printf("%d: %08x \"%s\"\n", event->wd, event->mask, event->len ? event->name : "");
        if(event->len) {
            strcpy(filename, event->name);
            if(event->mask & IN_CREATE) {
                open_device(devname, print_flags);
            }
            else {
                close_device(devname, print_flags);
            }
        }
        event_size = sizeof(*event) + event->len;
        res -= event_size;
        event_pos += event_size;
    }
    return 0;
}

static int scan_dir(const char *dirname, int print_flags)
{
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strcpy(filename, de->d_name);
        if (!device_in_filter(devname))
            continue;
        open_device(devname, print_flags);
    }
    closedir(dir);
    return 0;
}

static void usage(char *name)
{
    fprintf(stderr, "Usage: %s [-t] [-n] [-s switchmask] [-S] [-v [mask]] [-d] [-p] [-i] [-l] [-f event_types] [-q] [-c count] [-r] [device]\n", name);
    fprintf(stderr, "    -t: show time stamps\n");
    fprintf(stderr, "    -n: don't print newlines\n");
    fprintf(stderr, "    -s: print switch states for given bits\n");
    fprintf(stderr, "    -S: print all switch states\n");
    fprintf(stderr, "    -v: verbosity mask (errs=1, dev=2, name=4, info=8, vers=16, pos. events=32, props=64)\n");
    fprintf(stderr, "    -d: show HID descriptor, if available\n");
    fprintf(stderr, "    -p: show possible events (errs, dev, name, pos. events)\n");
    fprintf(stderr, "    -i: show all device info and possible events\n");
    fprintf(stderr, "    -l: label event types and names in plain text\n");
    fprintf(stderr, "    -f: filter event types (comma-separated: EV_SYN,EV_KEY,EV_REL,EV_ABS,EV_MSC,EV_SW,EV_LED,EV_SND,EV_REP,EV_FF,EV_PWR,EV_FF_STATUS,BST_EV_ABS_AND_KEY)\n");
    fprintf(stderr, "    -q: quiet (clear verbosity mask)\n");
    fprintf(stderr, "    -c: print given number of events then exit\n");
    fprintf(stderr, "    -r: print rate events are received\n");
    fprintf(stderr, "    -D: comma-separated list of full device paths to listen (e.g. -D \"/dev/input/event2,/dev/input/event4\")\n");
}

int getevent_main(int argc, char *argv[])
{
    int c;
    int i;
    int res;
    int get_time = 0;
    int print_device = 0;
    char *newline = "\n";
    uint16_t get_switch = 0;
    struct input_event event;
    int print_flags = 0;
    int print_flags_set = 0;
    int dont_block = -1;
    int event_count = 0;
    int sync_rate = 0;
    int64_t last_sync_time = 0;
    const char *device = NULL;
    const char *device_path = "/dev/input";

    /* disable buffering on stdout */
    setbuf(stdout, NULL);

    opterr = 0;
    do {
        c = getopt(argc, argv, "tns:Sv::dpilf:c:rqhD:");
        if (c == EOF)
            break;
        switch (c) {
        case 't':
            get_time = 1;
            break;
        case 'n':
            newline = "";
            break;
        case 's':
            get_switch = strtoul(optarg, NULL, 0);
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'S':
            get_switch = ~0;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'v':
            if(optarg)
                print_flags |= strtoul(optarg, NULL, 0);
            else
                print_flags |= PRINT_DEVICE | PRINT_DEVICE_NAME | PRINT_DEVICE_INFO | PRINT_VERSION;
            print_flags_set = 1;
            break;
        case 'd':
            print_flags |= PRINT_HID_DESCRIPTOR;
            break;
        case 'D': {
            char *token, *saveptr;
            char *list = strdup(optarg);
            if (!list) {
                fprintf(stderr, "Memory allocation failed for -D argument\n");
                exit(1);
            }

            token = strtok_r(list, ",", &saveptr);
            while (token) {
                device_filter_paths = realloc(device_filter_paths, sizeof(char*) * (device_filter_count + 1));
                if (!device_filter_paths) {
                    fprintf(stderr, "Out of memory parsing -D argument\n");
                    exit(1);
                }
                device_filter_paths[device_filter_count++] = strdup(token);
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(list);
            break;
        }
        case 'p':
            print_flags |= PRINT_DEVICE_ERRORS | PRINT_DEVICE
                    | PRINT_DEVICE_NAME | PRINT_POSSIBLE_EVENTS | PRINT_INPUT_PROPS;
            print_flags_set = 1;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'i':
            print_flags |= PRINT_ALL_INFO;
            print_flags_set = 1;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'l':
            print_flags |= PRINT_LABELS;
            break;
        case 'f':
            if(parse_event_types(optarg) < 0) {
                usage(argv[0]);
                exit(1);
            }
            break;
        case 'q':
            print_flags_set = 1;
            break;
        case 'c':
            event_count = atoi(optarg);
            dont_block = 0;
            break;
        case 'r':
            sync_rate = 1;
            break;
        case '?':
            fprintf(stderr, "%s: invalid option -%c\n",
                argv[0], optopt);
        case 'h':
            usage(argv[0]);
            exit(1);
        }
    } while (1);
    if(dont_block == -1)
        dont_block = 0;

    if (optind + 1 == argc) {
        device = argv[optind];
        optind++;
    }
    if (optind != argc) {
        usage(argv[0]);
        exit(1);
    }
    nfds = 1;
    ufds = calloc(1, sizeof(ufds[0]));
    ufds[0].fd = inotify_init();
    ufds[0].events = POLLIN;
    print_startup_timestamp();
    if(device) {
        if(!print_flags_set)
            print_flags |= PRINT_DEVICE_ERRORS;
        res = open_device(device, print_flags);
        if(res < 0) {
            return 1;
        }
    } else {
        if(!print_flags_set)
            print_flags |= PRINT_DEVICE_ERRORS | PRINT_DEVICE | PRINT_DEVICE_NAME;
        print_device = 1;
		res = inotify_add_watch(ufds[0].fd, device_path, IN_DELETE | IN_CREATE);
        if(res < 0) {
            fprintf(stderr, "could not add watch for %s, %s\n", device_path, strerror(errno));
            return 1;
        }
        res = scan_dir(device_path, print_flags);
        if(res < 0) {
            fprintf(stderr, "scan dir failed for %s\n", device_path);
            return 1;
        }
    }

    if(get_switch) {
        for(i = 1; i < nfds; i++) {
            uint16_t sw;
            res = ioctl(ufds[i].fd, EVIOCGSW(1), &sw);
            if(res < 0) {
                fprintf(stderr, "could not get switch state, %s\n", strerror(errno));
                return 1;
            }
            sw &= get_switch;
            printf("%04x%s", sw, newline);
        }
    }

    if(dont_block)
        return 0;

    while(1) {
        //int pollres =
        poll(ufds, nfds, -1);
        //printf("poll %d, returned %d\n", nfds, pollres);
        if(ufds[0].revents & POLLIN) {
            read_notify(device_path, ufds[0].fd, print_flags);
        }
        for(i = 1; i < nfds; i++) {
            if(ufds[i].revents) {
                if(ufds[i].revents & POLLIN) {
                    res = read(ufds[i].fd, &event, sizeof(event));
                    if(res < (int)sizeof(event)) {
                        fprintf(stderr, "could not get evdev event, %s\n", strerror(errno));
                        return 1;
                    }

                    // Apply event filtering and Bluestacks mouse handling if enabled
                    if(event_filter_enabled) {
                        // Check basic event type filter
                        if(!(event_filter_mask & (1 << event.type))) {
                            continue;
                        }

                        // Ignore ABS_GAS and ABS_BRAKE events globally when BST_EV_ABS_AND_KEY is enabled
                        if((event_filter_mask & (1 << BST_EV_ABS_AND_KEY)) && event.type == EV_ABS &&
                            (event.code == ABS_GAS || event.code == ABS_BRAKE)) {
                            continue;
                        }

                        // Track mouse button state for Mouse Devices
                        struct mouse_device_state *mouse_state = find_mouse_device(i);
                        if(mouse_state != NULL) {
                            int is_drag_end = 0;

                            if((event_filter_mask & (1 << BST_EV_ABS_AND_KEY))) {
                                if(event.type == EV_ABS) {
                                    // Update last known coordinates
                                    if(event.code == ABS_X) {
                                        mouse_state->last_x = event.value;
                                    } else if(event.code == ABS_Y) {
                                        mouse_state->last_y = event.value;
                                    }
                                }
                                else if(event.type == EV_KEY) {
                                    // Check for any mouse button (BTN_MOUSE is same as BTN_LEFT)
                                    if(event.code == BTN_LEFT || event.code == BTN_RIGHT ||
                                    event.code == BTN_MIDDLE || event.code == BTN_SIDE ||
                                    event.code == BTN_EXTRA || event.code == BTN_MOUSE) {
                                        if(event.value == 0) { // Button up (released)
                                            mouse_state->buttons_pressed--;
                                            if(mouse_state->buttons_pressed < 0)
                                                mouse_state->buttons_pressed = 0;

                                            // Check if this release just ended the drag (buttons now == 0)
                                            if(mouse_state->buttons_pressed == 0) {
                                                is_drag_end = 1;
                                                // Clear skip_sync_event so SYN_REPORT after button release is not suppressed
                                                // This ensures SYN_REPORT after button release is not suppressed
                                                mouse_state->skip_sync_event = 0;
                                            }
                                        } else { // Button down (pressed)
                                            mouse_state->buttons_pressed++;
                                            // Clear skip_sync_event so SYN_REPORT after button press is not suppressed
                                            mouse_state->skip_sync_event = 0;
                                            // Generate fake events with last mouse position when button is pressed
                                            if(mouse_state->last_x != -1 && mouse_state->last_y != -1) {
                                                // Generate EV_ABS ABS_X event
                                                if(get_time) {
                                                    printf("[%8ld.%06ld] ", event.time.tv_sec, event.time.tv_usec);
                                                }
                                                if(print_device)
                                                    printf("%s: ", device_names[i]);
                                                print_event(EV_ABS, ABS_X, mouse_state->last_x, print_flags);
                                                printf("%s", newline);

                                                // Generate EV_ABS ABS_Y event
                                                if(get_time) {
                                                    printf("[%8ld.%06ld] ", event.time.tv_sec, event.time.tv_usec);
                                                }
                                                if(print_device)
                                                    printf("%s: ", device_names[i]);
                                                print_event(EV_ABS, ABS_Y, mouse_state->last_y, print_flags);
                                                printf("%s", newline);
                                            }
                                        }
                                    }
                                }
                                // Only suppress coordinate events during hover (no buttons pressed)
                                // Exception: allow events when this is a drag end
                                if(mouse_state->buttons_pressed == 0 && !is_drag_end) {
                                    // Skip ABS X/Y coordinate events during hover
                                    if(event.type == EV_ABS && (event.code == ABS_X || event.code == ABS_Y)) {
                                        mouse_state->skip_sync_event = 1;
                                        continue;
                                    }
                                }
                                // Suppress SYN_REPORT only if it's from hover coordinate suppression
                                // Don't suppress if this is a drag end (button just released)
                                if(event.type == EV_SYN && event.code == SYN_REPORT &&
                                   mouse_state->skip_sync_event && !is_drag_end) {
                                    mouse_state->skip_sync_event = 0;
                                    continue;
                                }
                            }
                        }
                    }

                    if(get_time) {
                        printf("[%8ld.%06ld] ", event.time.tv_sec, event.time.tv_usec);
                    }
                    if(print_device)
                        printf("%s: ", device_names[i]);
                    print_event(event.type, event.code, event.value, print_flags);
                    if(sync_rate && event.type == 0 && event.code == 0) {
                        int64_t now = event.time.tv_sec * 1000000LL + event.time.tv_usec;
                        if(last_sync_time)
                            printf(" rate %lld", 1000000LL / (now - last_sync_time));
                        last_sync_time = now;
                    }
                    printf("%s", newline);
                    if(event_count && --event_count == 0)
                        return 0;
                }
            }
        }
    }

    return 0;
}

#ifdef GETEVENT_WITH_MAIN
int main(int argc, char *argv[]) {
    return getevent_main(argc, argv);
}
#endif
