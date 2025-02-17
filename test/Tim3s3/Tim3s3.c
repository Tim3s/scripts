// https://syzkaller.appspot.com/bug?id=53e398b93d719792b5c397b6a408064711cc71ef
// autogenerated by syzkaller (https://github.com/google/syzkaller)

#define _GNU_SOURCE

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/usb/ch9.h>

static unsigned long long procid;

static void sleep_ms(uint64_t ms)
{
  usleep(ms * 1000);
}

static uint64_t current_time_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    exit(1);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool write_file(const char* file, const char* what, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, what);
  vsnprintf(buf, sizeof(buf), what, args);
  va_end(args);
  buf[sizeof(buf) - 1] = 0;
  int len = strlen(buf);
  int fd = open(file, O_WRONLY | O_CLOEXEC);
  if (fd == -1)
    return false;
  if (write(fd, buf, len) != len) {
    int err = errno;
    close(fd);
    errno = err;
    return false;
  }
  close(fd);
  return true;
}

#define MAX_FDS 30

#define USB_MAX_IFACE_NUM 4
#define USB_MAX_EP_NUM 32
#define USB_MAX_FDS 6

struct usb_endpoint_index {
  struct usb_endpoint_descriptor desc;
  int handle;
};

struct usb_iface_index {
  struct usb_interface_descriptor* iface;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bInterfaceClass;
  struct usb_endpoint_index eps[USB_MAX_EP_NUM];
  int eps_num;
};

struct usb_device_index {
  struct usb_device_descriptor* dev;
  struct usb_config_descriptor* config;
  uint8_t bDeviceClass;
  uint8_t bMaxPower;
  int config_length;
  struct usb_iface_index ifaces[USB_MAX_IFACE_NUM];
  int ifaces_num;
  int iface_cur;
};

struct usb_info {
  int fd;
  struct usb_device_index index;
};

static struct usb_info usb_devices[USB_MAX_FDS];
static int usb_devices_num;

static bool parse_usb_descriptor(const char* buffer, size_t length,
                                 struct usb_device_index* index)
{
  if (length < sizeof(*index->dev) + sizeof(*index->config))
    return false;
  memset(index, 0, sizeof(*index));
  index->dev = (struct usb_device_descriptor*)buffer;
  index->config = (struct usb_config_descriptor*)(buffer + sizeof(*index->dev));
  index->bDeviceClass = index->dev->bDeviceClass;
  index->bMaxPower = index->config->bMaxPower;
  index->config_length = length - sizeof(*index->dev);
  index->iface_cur = -1;
  size_t offset = 0;
  while (true) {
    if (offset + 1 >= length)
      break;
    uint8_t desc_length = buffer[offset];
    uint8_t desc_type = buffer[offset + 1];
    if (desc_length <= 2)
      break;
    if (offset + desc_length > length)
      break;
    if (desc_type == USB_DT_INTERFACE &&
        index->ifaces_num < USB_MAX_IFACE_NUM) {
      struct usb_interface_descriptor* iface =
          (struct usb_interface_descriptor*)(buffer + offset);
      index->ifaces[index->ifaces_num].iface = iface;
      index->ifaces[index->ifaces_num].bInterfaceNumber =
          iface->bInterfaceNumber;
      index->ifaces[index->ifaces_num].bAlternateSetting =
          iface->bAlternateSetting;
      index->ifaces[index->ifaces_num].bInterfaceClass = iface->bInterfaceClass;
      index->ifaces_num++;
    }
    if (desc_type == USB_DT_ENDPOINT && index->ifaces_num > 0) {
      struct usb_iface_index* iface = &index->ifaces[index->ifaces_num - 1];
      if (iface->eps_num < USB_MAX_EP_NUM) {
        memcpy(&iface->eps[iface->eps_num].desc, buffer + offset,
               sizeof(iface->eps[iface->eps_num].desc));
        iface->eps_num++;
      }
    }
    offset += desc_length;
  }
  return true;
}

static struct usb_device_index* add_usb_index(int fd, const char* dev,
                                              size_t dev_len)
{
  int i = __atomic_fetch_add(&usb_devices_num, 1, __ATOMIC_RELAXED);
  if (i >= USB_MAX_FDS)
    return NULL;
  if (!parse_usb_descriptor(dev, dev_len, &usb_devices[i].index))
    return NULL;
  __atomic_store_n(&usb_devices[i].fd, fd, __ATOMIC_RELEASE);
  return &usb_devices[i].index;
}

static struct usb_device_index* lookup_usb_index(int fd)
{
  for (int i = 0; i < USB_MAX_FDS; i++) {
    if (__atomic_load_n(&usb_devices[i].fd, __ATOMIC_ACQUIRE) == fd)
      return &usb_devices[i].index;
  }
  return NULL;
}

struct vusb_connect_string_descriptor {
  uint32_t len;
  char* str;
} __attribute__((packed));

struct vusb_connect_descriptors {
  uint32_t qual_len;
  char* qual;
  uint32_t bos_len;
  char* bos;
  uint32_t strs_len;
  struct vusb_connect_string_descriptor strs[0];
} __attribute__((packed));

static const char default_string[] = {8, USB_DT_STRING, 's', 0, 'y', 0, 'z', 0};

static const char default_lang_id[] = {4, USB_DT_STRING, 0x09, 0x04};

static bool
lookup_connect_response_in(int fd, const struct vusb_connect_descriptors* descs,
                           const struct usb_ctrlrequest* ctrl,
                           char** response_data, uint32_t* response_length)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  uint8_t str_idx;
  if (!index)
    return false;
  switch (ctrl->bRequestType & USB_TYPE_MASK) {
  case USB_TYPE_STANDARD:
    switch (ctrl->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
      switch (ctrl->wValue >> 8) {
      case USB_DT_DEVICE:
        *response_data = (char*)index->dev;
        *response_length = sizeof(*index->dev);
        return true;
      case USB_DT_CONFIG:
        *response_data = (char*)index->config;
        *response_length = index->config_length;
        return true;
      case USB_DT_STRING:
        str_idx = (uint8_t)ctrl->wValue;
        if (descs && str_idx < descs->strs_len) {
          *response_data = descs->strs[str_idx].str;
          *response_length = descs->strs[str_idx].len;
          return true;
        }
        if (str_idx == 0) {
          *response_data = (char*)&default_lang_id[0];
          *response_length = default_lang_id[0];
          return true;
        }
        *response_data = (char*)&default_string[0];
        *response_length = default_string[0];
        return true;
      case USB_DT_BOS:
        *response_data = descs->bos;
        *response_length = descs->bos_len;
        return true;
      case USB_DT_DEVICE_QUALIFIER:
        if (!descs->qual) {
          struct usb_qualifier_descriptor* qual =
              (struct usb_qualifier_descriptor*)response_data;
          qual->bLength = sizeof(*qual);
          qual->bDescriptorType = USB_DT_DEVICE_QUALIFIER;
          qual->bcdUSB = index->dev->bcdUSB;
          qual->bDeviceClass = index->dev->bDeviceClass;
          qual->bDeviceSubClass = index->dev->bDeviceSubClass;
          qual->bDeviceProtocol = index->dev->bDeviceProtocol;
          qual->bMaxPacketSize0 = index->dev->bMaxPacketSize0;
          qual->bNumConfigurations = index->dev->bNumConfigurations;
          qual->bRESERVED = 0;
          *response_length = sizeof(*qual);
          return true;
        }
        *response_data = descs->qual;
        *response_length = descs->qual_len;
        return true;
      default:
        break;
      }
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return false;
}

typedef bool (*lookup_connect_out_response_t)(
    int fd, const struct vusb_connect_descriptors* descs,
    const struct usb_ctrlrequest* ctrl, bool* done);

static bool lookup_connect_response_out_generic(
    int fd, const struct vusb_connect_descriptors* descs,
    const struct usb_ctrlrequest* ctrl, bool* done)
{
  switch (ctrl->bRequestType & USB_TYPE_MASK) {
  case USB_TYPE_STANDARD:
    switch (ctrl->bRequest) {
    case USB_REQ_SET_CONFIGURATION:
      *done = true;
      return true;
    default:
      break;
    }
    break;
  }
  return false;
}

#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
  __u8 driver_name[UDC_NAME_LENGTH_MAX];
  __u8 device_name[UDC_NAME_LENGTH_MAX];
  __u8 speed;
};

enum usb_raw_event_type {
  USB_RAW_EVENT_INVALID = 0,
  USB_RAW_EVENT_CONNECT = 1,
  USB_RAW_EVENT_CONTROL = 2,
};

struct usb_raw_event {
  __u32 type;
  __u32 length;
  __u8 data[0];
};

struct usb_raw_ep_io {
  __u16 ep;
  __u16 flags;
  __u32 length;
  __u8 data[0];
};

#define USB_RAW_EPS_NUM_MAX 30
#define USB_RAW_EP_NAME_MAX 16
#define USB_RAW_EP_ADDR_ANY 0xff

struct usb_raw_ep_caps {
  __u32 type_control : 1;
  __u32 type_iso : 1;
  __u32 type_bulk : 1;
  __u32 type_int : 1;
  __u32 dir_in : 1;
  __u32 dir_out : 1;
};

struct usb_raw_ep_limits {
  __u16 maxpacket_limit;
  __u16 max_streams;
  __u32 reserved;
};

struct usb_raw_ep_info {
  __u8 name[USB_RAW_EP_NAME_MAX];
  __u32 addr;
  struct usb_raw_ep_caps caps;
  struct usb_raw_ep_limits limits;
};

struct usb_raw_eps_info {
  struct usb_raw_ep_info eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE _IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO _IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL _IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT _IOW('U', 13, __u32)
#define USB_RAW_IOCTL_EP_CLEAR_HALT _IOW('U', 14, __u32)
#define USB_RAW_IOCTL_EP_SET_WEDGE _IOW('U', 15, __u32)

static int usb_raw_open()
{
  return open("/dev/raw-gadget", O_RDWR);
}

static int usb_raw_init(int fd, uint32_t speed, const char* driver,
                        const char* device)
{
  struct usb_raw_init arg;
  strncpy((char*)&arg.driver_name[0], driver, sizeof(arg.driver_name));
  strncpy((char*)&arg.device_name[0], device, sizeof(arg.device_name));
  arg.speed = speed;
  return ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
}

static int usb_raw_run(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_RUN, 0);
}

static int usb_raw_event_fetch(int fd, struct usb_raw_event* event)
{
  return ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
}

static int usb_raw_ep0_write(int fd, struct usb_raw_ep_io* io)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
}

static int usb_raw_ep0_read(int fd, struct usb_raw_ep_io* io)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
}

static int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor* desc)
{
  return ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
}

static int usb_raw_ep_disable(int fd, int ep)
{
  return ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep);
}

static int usb_raw_configure(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
}

static int usb_raw_vbus_draw(int fd, uint32_t power)
{
  return ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
}

static int usb_raw_ep0_stall(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
}

static void set_interface(int fd, int n)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  if (!index)
    return;
  if (index->iface_cur >= 0 && index->iface_cur < index->ifaces_num) {
    for (int ep = 0; ep < index->ifaces[index->iface_cur].eps_num; ep++) {
      int rv = usb_raw_ep_disable(
          fd, index->ifaces[index->iface_cur].eps[ep].handle);
      if (rv < 0) {
      } else {
      }
    }
  }
  if (n >= 0 && n < index->ifaces_num) {
    for (int ep = 0; ep < index->ifaces[n].eps_num; ep++) {
      int rv = usb_raw_ep_enable(fd, &index->ifaces[n].eps[ep].desc);
      if (rv < 0) {
      } else {
        index->ifaces[n].eps[ep].handle = rv;
      }
    }
    index->iface_cur = n;
  }
}

static int configure_device(int fd)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  if (!index)
    return -1;
  int rv = usb_raw_vbus_draw(fd, index->bMaxPower);
  if (rv < 0) {
    return rv;
  }
  rv = usb_raw_configure(fd);
  if (rv < 0) {
    return rv;
  }
  set_interface(fd, 0);
  return 0;
}

#define USB_MAX_PACKET_SIZE 4096

struct usb_raw_control_event {
  struct usb_raw_event inner;
  struct usb_ctrlrequest ctrl;
  char data[USB_MAX_PACKET_SIZE];
};

struct usb_raw_ep_io_data {
  struct usb_raw_ep_io inner;
  char data[USB_MAX_PACKET_SIZE];
};

static volatile long
syz_usb_connect_impl(uint64_t speed, uint64_t dev_len, const char* dev,
                     const struct vusb_connect_descriptors* descs,
                     lookup_connect_out_response_t lookup_connect_response_out)
{
  if (!dev) {
    return -1;
  }
  int fd = usb_raw_open();
  if (fd < 0) {
    return fd;
  }
  if (fd >= MAX_FDS) {
    close(fd);
    return -1;
  }
  struct usb_device_index* index = add_usb_index(fd, dev, dev_len);
  if (!index) {
    return -1;
  }
  char device[32];
  sprintf(&device[0], "dummy_udc.%llu", procid);
  int rv = usb_raw_init(fd, speed, "dummy_udc", &device[0]);
  if (rv < 0) {
    return rv;
  }
  rv = usb_raw_run(fd);
  if (rv < 0) {
    return rv;
  }
  bool done = false;
  while (!done) {
    struct usb_raw_control_event event;
    event.inner.type = 0;
    event.inner.length = sizeof(event.ctrl);
    rv = usb_raw_event_fetch(fd, (struct usb_raw_event*)&event);
    if (rv < 0) {
      return rv;
    }
    if (event.inner.type != USB_RAW_EVENT_CONTROL)
      continue;
    char* response_data = NULL;
    uint32_t response_length = 0;
    if (event.ctrl.bRequestType & USB_DIR_IN) {
      if (!lookup_connect_response_in(fd, descs, &event.ctrl, &response_data,
                                      &response_length)) {
        usb_raw_ep0_stall(fd);
        continue;
      }
    } else {
      if (!lookup_connect_response_out(fd, descs, &event.ctrl, &done)) {
        usb_raw_ep0_stall(fd);
        continue;
      }
      response_data = NULL;
      response_length = event.ctrl.wLength;
    }
    if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
        event.ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
      rv = configure_device(fd);
      if (rv < 0) {
        return rv;
      }
    }
    struct usb_raw_ep_io_data response;
    response.inner.ep = 0;
    response.inner.flags = 0;
    if (response_length > sizeof(response.data))
      response_length = 0;
    if (event.ctrl.wLength < response_length)
      response_length = event.ctrl.wLength;
    response.inner.length = response_length;
    if (response_data)
      memcpy(&response.data[0], response_data, response_length);
    else
      memset(&response.data[0], 0, response_length);
    if (event.ctrl.bRequestType & USB_DIR_IN) {
      rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io*)&response);
    } else {
      rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io*)&response);
    }
    if (rv < 0) {
      return rv;
    }
  }
  sleep_ms(200);
  return fd;
}

static volatile long syz_usb_connect(volatile long a0, volatile long a1,
                                     volatile long a2, volatile long a3)
{
  uint64_t speed = a0;
  uint64_t dev_len = a1;
  const char* dev = (const char*)a2;
  const struct vusb_connect_descriptors* descs =
      (const struct vusb_connect_descriptors*)a3;
  return syz_usb_connect_impl(speed, dev_len, dev, descs,
                              &lookup_connect_response_out_generic);
}

static void kill_and_wait(int pid, int* status)
{
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  for (int i = 0; i < 100; i++) {
    if (waitpid(-1, status, WNOHANG | __WALL) == pid)
      return;
    usleep(1000);
  }
  DIR* dir = opendir("/sys/fs/fuse/connections");
  if (dir) {
    for (;;) {
      struct dirent* ent = readdir(dir);
      if (!ent)
        break;
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      char abort[300];
      snprintf(abort, sizeof(abort), "/sys/fs/fuse/connections/%s/abort",
               ent->d_name);
      int fd = open(abort, O_WRONLY);
      if (fd == -1) {
        continue;
      }
      if (write(fd, abort, 1) < 0) {
      }
      close(fd);
    }
    closedir(dir);
  } else {
  }
  while (waitpid(-1, status, __WALL) != pid) {
  }
}

static void setup_test()
{
  prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
  setpgrp();
  write_file("/proc/self/oom_score_adj", "1000");
}

static void execute_one(void);

#define WAIT_FLAGS __WALL

static void loop(void)
{
  int iter = 0;
  for (;; iter++) {
    int pid = fork();
    if (pid < 0)
      exit(1);
    if (pid == 0) {
      setup_test();
      execute_one();
      exit(0);
    }
    int status = 0;
    uint64_t start = current_time_ms();
    for (;;) {
      if (waitpid(-1, &status, WNOHANG | WAIT_FLAGS) == pid)
        break;
      sleep_ms(1);
      if (current_time_ms() - start < 5000)
        continue;
      kill_and_wait(pid, &status);
      break;
    }
  }
}

void execute_one(void)
{
  *(uint8_t*)0x20000000 = 0x12;
  *(uint8_t*)0x20000001 = 1;
  *(uint16_t*)0x20000002 = 0x250;
  *(uint8_t*)0x20000004 = 0xa3;
  *(uint8_t*)0x20000005 = 0xf5;
  *(uint8_t*)0x20000006 = 0x34;
  *(uint8_t*)0x20000007 = 0x10;
  *(uint16_t*)0x20000008 = 0x1286;
  *(uint16_t*)0x2000000a = 0x2049;
  *(uint16_t*)0x2000000c = 0xe53a;
  *(uint8_t*)0x2000000e = 1;
  *(uint8_t*)0x2000000f = 2;
  *(uint8_t*)0x20000010 = 3;
  *(uint8_t*)0x20000011 = 1;
  *(uint8_t*)0x20000012 = 9;
  *(uint8_t*)0x20000013 = 2;
  *(uint16_t*)0x20000014 = 0x146;
  *(uint8_t*)0x20000016 = 3;
  *(uint8_t*)0x20000017 = 4;
  *(uint8_t*)0x20000018 = 8;
  *(uint8_t*)0x20000019 = 0x60;
  *(uint8_t*)0x2000001a = 6;
  *(uint8_t*)0x2000001b = 9;
  *(uint8_t*)0x2000001c = 4;
  *(uint8_t*)0x2000001d = 0xae;
  *(uint8_t*)0x2000001e = 0xb4;
  *(uint8_t*)0x2000001f = 7;
  *(uint8_t*)0x20000020 = 0x2f;
  *(uint8_t*)0x20000021 = 0xfa;
  *(uint8_t*)0x20000022 = 0x5a;
  *(uint8_t*)0x20000023 = 0x81;
  *(uint8_t*)0x20000024 = 0xa;
  *(uint8_t*)0x20000025 = 0x24;
  *(uint8_t*)0x20000026 = 1;
  *(uint16_t*)0x20000027 = 0x1f;
  *(uint8_t*)0x20000029 = 9;
  *(uint8_t*)0x2000002a = 2;
  *(uint8_t*)0x2000002b = 1;
  *(uint8_t*)0x2000002c = 2;
  *(uint8_t*)0x2000002d = 7;
  *(uint8_t*)0x2000002e = 0x24;
  *(uint8_t*)0x2000002f = 7;
  *(uint8_t*)0x20000030 = 2;
  *(uint16_t*)0x20000031 = 1;
  *(uint8_t*)0x20000033 = 2;
  *(uint8_t*)0x20000034 = 7;
  *(uint8_t*)0x20000035 = 0x24;
  *(uint8_t*)0x20000036 = 8;
  *(uint8_t*)0x20000037 = 3;
  *(uint16_t*)0x20000038 = 0x3ff;
  *(uint8_t*)0x2000003a = 0xfd;
  *(uint8_t*)0x2000003b = 9;
  *(uint8_t*)0x2000003c = 0x24;
  *(uint8_t*)0x2000003d = 3;
  *(uint8_t*)0x2000003e = 6;
  *(uint16_t*)0x2000003f = 0x107;
  *(uint8_t*)0x20000041 = 2;
  *(uint8_t*)0x20000042 = 2;
  *(uint8_t*)0x20000043 = 0x20;
  *(uint8_t*)0x20000044 = 7;
  *(uint8_t*)0x20000045 = 0x24;
  *(uint8_t*)0x20000046 = 7;
  *(uint8_t*)0x20000047 = 5;
  *(uint16_t*)0x20000048 = 0;
  *(uint8_t*)0x2000004a = 0x20;
  *(uint8_t*)0x2000004b = 7;
  *(uint8_t*)0x2000004c = 0x24;
  *(uint8_t*)0x2000004d = 7;
  *(uint8_t*)0x2000004e = 2;
  *(uint16_t*)0x2000004f = 4;
  *(uint8_t*)0x20000051 = 3;
  *(uint8_t*)0x20000052 = 9;
  *(uint8_t*)0x20000053 = 5;
  *(uint8_t*)0x20000054 = 0xb;
  *(uint8_t*)0x20000055 = 0;
  *(uint16_t*)0x20000056 = 0x400;
  *(uint8_t*)0x20000058 = 0xec;
  *(uint8_t*)0x20000059 = 4;
  *(uint8_t*)0x2000005a = 8;
  *(uint8_t*)0x2000005b = 9;
  *(uint8_t*)0x2000005c = 5;
  *(uint8_t*)0x2000005d = 0xa;
  *(uint8_t*)0x2000005e = 3;
  *(uint16_t*)0x2000005f = 0x10;
  *(uint8_t*)0x20000061 = 0xbe;
  *(uint8_t*)0x20000062 = 1;
  *(uint8_t*)0x20000063 = 0x16;
  *(uint8_t*)0x20000064 = 7;
  *(uint8_t*)0x20000065 = 0x25;
  *(uint8_t*)0x20000066 = 1;
  *(uint8_t*)0x20000067 = 0;
  *(uint8_t*)0x20000068 = 4;
  *(uint16_t*)0x20000069 = 0x3ff;
  *(uint8_t*)0x2000006b = 9;
  *(uint8_t*)0x2000006c = 5;
  *(uint8_t*)0x2000006d = 0xb;
  *(uint8_t*)0x2000006e = 3;
  *(uint16_t*)0x2000006f = 0x3ff;
  *(uint8_t*)0x20000071 = 5;
  *(uint8_t*)0x20000072 = 0x7f;
  *(uint8_t*)0x20000073 = 0x90;
  *(uint8_t*)0x20000074 = 9;
  *(uint8_t*)0x20000075 = 5;
  *(uint8_t*)0x20000076 = 0xe;
  *(uint8_t*)0x20000077 = 3;
  *(uint16_t*)0x20000078 = 0x20;
  *(uint8_t*)0x2000007a = 0x20;
  *(uint8_t*)0x2000007b = 0x7f;
  *(uint8_t*)0x2000007c = 1;
  *(uint8_t*)0x2000007d = 7;
  *(uint8_t*)0x2000007e = 0x25;
  *(uint8_t*)0x2000007f = 1;
  *(uint8_t*)0x20000080 = 3;
  *(uint8_t*)0x20000081 = 5;
  *(uint16_t*)0x20000082 = 0xf725;
  *(uint8_t*)0x20000084 = 7;
  *(uint8_t*)0x20000085 = 0x25;
  *(uint8_t*)0x20000086 = 1;
  *(uint8_t*)0x20000087 = 1;
  *(uint8_t*)0x20000088 = 0x7f;
  *(uint16_t*)0x20000089 = 5;
  *(uint8_t*)0x2000008b = 9;
  *(uint8_t*)0x2000008c = 5;
  *(uint8_t*)0x2000008d = 8;
  *(uint8_t*)0x2000008e = 0x10;
  *(uint16_t*)0x2000008f = 0x10;
  *(uint8_t*)0x20000091 = 7;
  *(uint8_t*)0x20000092 = 2;
  *(uint8_t*)0x20000093 = 0x80;
  *(uint8_t*)0x20000094 = 2;
  *(uint8_t*)0x20000095 = 3;
  *(uint8_t*)0x20000096 = 7;
  *(uint8_t*)0x20000097 = 0x25;
  *(uint8_t*)0x20000098 = 1;
  *(uint8_t*)0x20000099 = 1;
  *(uint8_t*)0x2000009a = 2;
  *(uint16_t*)0x2000009b = 0xa89b;
  *(uint8_t*)0x2000009d = 9;
  *(uint8_t*)0x2000009e = 5;
  *(uint8_t*)0x2000009f = 0;
  *(uint8_t*)0x200000a0 = 0x1e;
  *(uint16_t*)0x200000a1 = 0x200;
  *(uint8_t*)0x200000a3 = 3;
  *(uint8_t*)0x200000a4 = 0x7d;
  *(uint8_t*)0x200000a5 = 8;
  *(uint8_t*)0x200000a6 = 9;
  *(uint8_t*)0x200000a7 = 5;
  *(uint8_t*)0x200000a8 = 8;
  *(uint8_t*)0x200000a9 = 0;
  *(uint16_t*)0x200000aa = 0x20;
  *(uint8_t*)0x200000ac = 5;
  *(uint8_t*)0x200000ad = 0x81;
  *(uint8_t*)0x200000ae = 4;
  *(uint8_t*)0x200000af = 2;
  *(uint8_t*)0x200000b0 = 0x11;
  *(uint8_t*)0x200000b1 = 2;
  *(uint8_t*)0x200000b2 = 0x30;
  *(uint8_t*)0x200000b3 = 9;
  *(uint8_t*)0x200000b4 = 4;
  *(uint8_t*)0x200000b5 = 0xfe;
  *(uint8_t*)0x200000b6 = 9;
  *(uint8_t*)0x200000b7 = 1;
  *(uint8_t*)0x200000b8 = 0xbc;
  *(uint8_t*)0x200000b9 = 0x26;
  *(uint8_t*)0x200000ba = 0x95;
  *(uint8_t*)0x200000bb = 0x3f;
  *(uint8_t*)0x200000bc = 9;
  *(uint8_t*)0x200000bd = 5;
  *(uint8_t*)0x200000be = 0xa;
  *(uint8_t*)0x200000bf = 0;
  *(uint16_t*)0x200000c0 = 0x400;
  *(uint8_t*)0x200000c2 = 9;
  *(uint8_t*)0x200000c3 = 3;
  *(uint8_t*)0x200000c4 = 1;
  *(uint8_t*)0x200000c5 = 7;
  *(uint8_t*)0x200000c6 = 0x25;
  *(uint8_t*)0x200000c7 = 1;
  *(uint8_t*)0x200000c8 = 0x82;
  *(uint8_t*)0x200000c9 = 9;
  *(uint16_t*)0x200000ca = 0;
  *(uint8_t*)0x200000cc = 9;
  *(uint8_t*)0x200000cd = 4;
  *(uint8_t*)0x200000ce = 0x19;
  *(uint8_t*)0x200000cf = 0x84;
  *(uint8_t*)0x200000d0 = 6;
  *(uint8_t*)0x200000d1 = 0xee;
  *(uint8_t*)0x200000d2 = 0x85;
  *(uint8_t*)0x200000d3 = 0x4f;
  *(uint8_t*)0x200000d4 = 1;
  *(uint8_t*)0x200000d5 = 0xa;
  *(uint8_t*)0x200000d6 = 0x24;
  *(uint8_t*)0x200000d7 = 1;
  *(uint16_t*)0x200000d8 = 2;
  *(uint8_t*)0x200000da = 0x1f;
  *(uint8_t*)0x200000db = 2;
  *(uint8_t*)0x200000dc = 1;
  *(uint8_t*)0x200000dd = 2;
  *(uint8_t*)0x200000de = 5;
  *(uint8_t*)0x200000df = 0x24;
  *(uint8_t*)0x200000e0 = 5;
  *(uint8_t*)0x200000e1 = 3;
  *(uint8_t*)0x200000e2 = 8;
  *(uint8_t*)0x200000e3 = 5;
  *(uint8_t*)0x200000e4 = 0x24;
  *(uint8_t*)0x200000e5 = 4;
  *(uint8_t*)0x200000e6 = 3;
  *(uint8_t*)0x200000e7 = 3;
  *(uint8_t*)0x200000e8 = 7;
  *(uint8_t*)0x200000e9 = 0x24;
  *(uint8_t*)0x200000ea = 7;
  *(uint8_t*)0x200000eb = 3;
  *(uint16_t*)0x200000ec = 1;
  *(uint8_t*)0x200000ee = 2;
  *(uint8_t*)0x200000ef = 9;
  *(uint8_t*)0x200000f0 = 0x24;
  *(uint8_t*)0x200000f1 = 3;
  *(uint8_t*)0x200000f2 = 3;
  *(uint16_t*)0x200000f3 = 0x303;
  *(uint8_t*)0x200000f5 = 5;
  *(uint8_t*)0x200000f6 = 6;
  *(uint8_t*)0x200000f7 = 7;
  *(uint8_t*)0x200000f8 = 7;
  *(uint8_t*)0x200000f9 = 0x24;
  *(uint8_t*)0x200000fa = 8;
  *(uint8_t*)0x200000fb = 6;
  *(uint16_t*)0x200000fc = 4;
  *(uint8_t*)0x200000fe = 4;
  *(uint8_t*)0x200000ff = 0xc;
  *(uint8_t*)0x20000100 = 0x24;
  *(uint8_t*)0x20000101 = 2;
  *(uint8_t*)0x20000102 = 3;
  *(uint16_t*)0x20000103 = 0x202;
  *(uint8_t*)0x20000105 = 4;
  *(uint8_t*)0x20000106 = 0x28;
  *(uint16_t*)0x20000107 = 1;
  *(uint8_t*)0x20000109 = 0x80;
  *(uint8_t*)0x2000010a = 0x80;
  *(uint8_t*)0x2000010b = 9;
  *(uint8_t*)0x2000010c = 5;
  *(uint8_t*)0x2000010d = 0xf;
  *(uint8_t*)0x2000010e = 0xc;
  *(uint16_t*)0x2000010f = 8;
  *(uint8_t*)0x20000111 = 3;
  *(uint8_t*)0x20000112 = 9;
  *(uint8_t*)0x20000113 = 6;
  *(uint8_t*)0x20000114 = 9;
  *(uint8_t*)0x20000115 = 5;
  *(uint8_t*)0x20000116 = 0xd;
  *(uint8_t*)0x20000117 = 8;
  *(uint16_t*)0x20000118 = 8;
  *(uint8_t*)0x2000011a = 9;
  *(uint8_t*)0x2000011b = 0xe2;
  *(uint8_t*)0x2000011c = 3;
  *(uint8_t*)0x2000011d = 7;
  *(uint8_t*)0x2000011e = 0x25;
  *(uint8_t*)0x2000011f = 1;
  *(uint8_t*)0x20000120 = 0x80;
  *(uint8_t*)0x20000121 = 0x81;
  *(uint16_t*)0x20000122 = 0;
  *(uint8_t*)0x20000124 = 9;
  *(uint8_t*)0x20000125 = 5;
  *(uint8_t*)0x20000126 = 9;
  *(uint8_t*)0x20000127 = 3;
  *(uint16_t*)0x20000128 = 0x200;
  *(uint8_t*)0x2000012a = 0xc7;
  *(uint8_t*)0x2000012b = 3;
  *(uint8_t*)0x2000012c = 0xb4;
  *(uint8_t*)0x2000012d = 9;
  *(uint8_t*)0x2000012e = 5;
  *(uint8_t*)0x2000012f = 0xd9;
  *(uint8_t*)0x20000130 = 0x10;
  *(uint16_t*)0x20000131 = 0x20;
  *(uint8_t*)0x20000133 = 3;
  *(uint8_t*)0x20000134 = 0x21;
  *(uint8_t*)0x20000135 = 7;
  *(uint8_t*)0x20000136 = 2;
  *(uint8_t*)0x20000137 = 1;
  *(uint8_t*)0x20000138 = 9;
  *(uint8_t*)0x20000139 = 5;
  *(uint8_t*)0x2000013a = 2;
  *(uint8_t*)0x2000013b = 2;
  *(uint16_t*)0x2000013c = 0;
  *(uint8_t*)0x2000013e = 0xad;
  *(uint8_t*)0x2000013f = 0xfe;
  *(uint8_t*)0x20000140 = 7;
  *(uint8_t*)0x20000141 = 7;
  *(uint8_t*)0x20000142 = 0x25;
  *(uint8_t*)0x20000143 = 1;
  *(uint8_t*)0x20000144 = 0;
  *(uint8_t*)0x20000145 = 6;
  *(uint16_t*)0x20000146 = 0x89ce;
  *(uint8_t*)0x20000148 = 9;
  *(uint8_t*)0x20000149 = 5;
  *(uint8_t*)0x2000014a = 0xd;
  *(uint8_t*)0x2000014b = 0x10;
  *(uint16_t*)0x2000014c = 0;
  *(uint8_t*)0x2000014e = 9;
  *(uint8_t*)0x2000014f = 0x40;
  *(uint8_t*)0x20000150 = 9;
  *(uint8_t*)0x20000151 = 7;
  *(uint8_t*)0x20000152 = 0x25;
  *(uint8_t*)0x20000153 = 1;
  *(uint8_t*)0x20000154 = 0;
  *(uint8_t*)0x20000155 = 1;
  *(uint16_t*)0x20000156 = 4;
  *(uint32_t*)0x20000400 = 0;
  *(uint64_t*)0x20000404 = 0;
  *(uint32_t*)0x2000040c = 0;
  *(uint64_t*)0x20000410 = 0;
  *(uint32_t*)0x20000418 = 1;
  *(uint32_t*)0x2000041c = 0;
  *(uint64_t*)0x20000420 = 0;
  syz_usb_connect(5, 0x158, 0x20000000, 0x20000400);
}
int main(void)
{
  syscall(__NR_mmap, 0x1ffff000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
  syscall(__NR_mmap, 0x20000000ul, 0x1000000ul, 7ul, 0x32ul, -1, 0ul);
  syscall(__NR_mmap, 0x21000000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
  loop();
  return 0;
}