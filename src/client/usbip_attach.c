/*
 * Copyright (C) 2017 La Ode Muh. Fadlun Akbar <fadlun.net@gmail.com>
 *               2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#include "usbip.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "vhci_driver.h"

#define MAX_BUFF 100
static int
record_connection(const char* host,
                  const char* port,
                  const char* busid,
                  int rhport)
{
    int fd;
    char path[PATH_MAX + 1];
    char buff[MAX_BUFF + 1];
    int ret;

    ret = mkdir(VHCI_STATE_PATH, 0700);
    if (ret < 0) {
        /* if VHCI_STATE_PATH exists, then it better be a directory */
        if (errno == EEXIST) {
            struct stat s;

            ret = stat(VHCI_STATE_PATH, &s);
            if (ret < 0)
                return -1;
            if (!(s.st_mode & S_IFDIR))
                return -1;
        } else
            return -1;
    }

    snprintf(path, PATH_MAX, VHCI_STATE_PATH "/port%d", rhport);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
    if (fd < 0)
        return -1;

    snprintf(buff, MAX_BUFF, "%s %s %s\n", host, port, busid);

    ret = write(fd, buff, strlen(buff));
    if (ret != (ssize_t)strlen(buff)) {
        close(fd);
        return -1;
    }

    close(fd);

    return 0;
}

static int
import_device(int sockfd, struct usbip_usb_device* udev)
{
    int rc;
    int port;

    rc = usbip_vhci_driver_open();
    if (rc < 0) {
        err("open vhci_driver");
        return -1;
    }

    port = usbip_vhci_get_free_port();
    if (port < 0) {
        err("no free port");
        usbip_vhci_driver_close();
        return -1;
    }

    rc = usbip_vhci_attach_device(
      port, sockfd, udev->busnum, udev->devnum, udev->speed);
    if (rc < 0) {
        err("import device");
        usbip_vhci_driver_close();
        return -1;
    }

    usbip_vhci_driver_close();

    return port;
}

static int
query_import_device(int sockfd, const char* busid)
{
    int rc;
    struct op_import_request request;
    struct op_import_reply reply;
    uint16_t code = OP_REP_IMPORT;

    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));

    /* send a request */
    rc = usbip_net_send_op_common(sockfd, OP_REQ_IMPORT, 0);
    if (rc < 0) {
        err("send op_common");
        return -1;
    }

    strncpy(request.busid, busid, SYSFS_BUS_ID_SIZE - 1);

    PACK_OP_IMPORT_REQUEST(0, &request);

    rc = usbip_net_send(sockfd, (void*)&request, sizeof(request));
    if (rc < 0) {
        err("send op_import_request");
        return -1;
    }

    /* receive a reply */
    rc = usbip_net_recv_op_common(sockfd, &code);
    if (rc < 0) {
        err("recv op_common");
        return -1;
    }

    rc = usbip_net_recv(sockfd, (void*)&reply, sizeof(reply));
    if (rc < 0) {
        err("recv op_import_reply");
        return -1;
    }

    PACK_OP_IMPORT_REPLY(0, &reply);

    /* check the reply */
    if (strncmp(reply.udev.busid, busid, SYSFS_BUS_ID_SIZE)) {
        err("recv different busid %s", reply.udev.busid);
        return -1;
    }

    /* import a device */
    return import_device(sockfd, &reply.udev);
}

int
check_device_state(const char* host, const char* busid)
{
    int sockfd;
    int rc;
    struct op_import_request request;
    uint16_t code = OP_REP_IMPORT;

    memset(&request, 0, sizeof(request));

    sockfd = usbip_net_tcp_connect(host, usbip_port_string);
    if (sockfd < 0) {
        err("tcp connect");
        return -1;
    }

    rc = usbip_net_send_op_common(sockfd, OP_REQ_IMPORT, 0);
    if (rc < 0) {
        err("send op_common");
        return -1;
    }

    strncpy(request.busid, busid, SYSFS_BUS_ID_SIZE - 1);

    PACK_OP_IMPORT_REQUEST(0, &request);

    rc = usbip_net_send(sockfd, (void*)&request, sizeof(request));
    if (rc < 0) {
        err("send op_import_request");
        return -1;
    }

    rc = usbip_net_recv_op_common(sockfd, &code);
    if (rc < 0) {
        err("recv op_common");
        return -1;
    }

    close(sockfd);
    return 0;
}

int
attach_device(const char* host, const char* busid)
{
    int sockfd;
    int rc;
    int rhport;

    sockfd = usbip_net_tcp_connect(host, usbip_port_string);
    if (sockfd < 0) {
        err("tcp connect");
        return -1;
    }

    rhport = query_import_device(sockfd, busid);
    if (rhport < 0) {
        err("query");
        return -1;
    }

    close(sockfd);

    printf("host: %s, usbip_port_string: %s, busid: %s, rhport: %d\n",
           host,
           usbip_port_string,
           busid,
           rhport);

    rc = record_connection(host, usbip_port_string, busid, rhport);
    if (rc < 0) {
        err("record connection");
        return -1;
    }

    return rhport;
}