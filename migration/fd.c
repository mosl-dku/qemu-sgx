/*
 * QEMU live migration via generic fd
 *
 * Copyright Red Hat, Inc. 2009-2016
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "channel.h"
#include "fd.h"
#include "migration.h"
#include "monitor/monitor.h"
#include "io/channel-util.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <pthread.h>


void fd_start_outgoing_migration(MigrationState *s, const char *fdname, Error **errp)
{
    QIOChannel *ioc;

    bool result = false;
    while(!result){
        printf("LOG : TRY receive finished\n");
        result = file_recv();
        if(!result){
            printf("LOG : recv failed...\n");
        }
    }

    if(access("/tmp/recv.dat", F_OK ) != -1) {
        printf("LOG : Create migration thread\n");
    } else {
        printf("LOG : File recv failed\n");
        return;
    }

    int fd = monitor_get_fd(cur_mon, fdname, errp);
    if (fd == -1) {
        return;
    }

    trace_migration_fd_outgoing(fd);
    ioc = qio_channel_new_fd(fd, errp);
    if (!ioc) {
        close(fd);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "migration-fd-outgoing");
    migration_channel_connect(s, ioc, NULL, NULL);
    object_unref(OBJECT(ioc));
}

bool file_recv(void){
int client_len;
    int client_sockfd;
    char buf[256];
    struct sockaddr_in clientaddr;

    char addr[20];
    printf("LOG : MIG : file_recv\n");
    FILE *fp = fopen("/tmp/mig_tgtip.tmp", "r");
    if(fgets(addr, sizeof(addr), fp)!= NULL)
        printf("LOG :  ip addr = %s\n", addr);
    fclose(fp);

    clientaddr.sin_family = AF_INET;
    clientaddr.sin_addr.s_addr = inet_addr(addr);
    clientaddr.sin_port = htons(8888);
    client_len = sizeof(clientaddr);

    client_sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (connect (client_sockfd, (struct sockaddr *)&clientaddr, client_len) < 0){
        printf("ERR : connect error\n");
        return false;
    }

    int nbyte;
    size_t filesize = 0, bufsize = 0;
    FILE *file = NULL;

    file = fopen("/tmp/recv.dat", "wb");

    ntohl(filesize);
    recv(client_sockfd, &filesize, sizeof(filesize), 0);
    //printf("file size = [%d]\n", filesize);
    bufsize = 256;
    while(filesize != 0){
        if(filesize < 256)
            bufsize = filesize;

        nbyte = recv(client_sockfd, buf, bufsize, 0);
        filesize = filesize -nbyte;

        fwrite(buf, sizeof(char), nbyte, file);

        nbyte = 0;

    }

    close(client_sockfd);
    fclose(file);
    return true;
}

static gboolean fd_accept_incoming_migration(QIOChannel *ioc,
                                             GIOCondition condition,
                                             gpointer opaque)
{
    migration_channel_process_incoming(ioc);
    object_unref(OBJECT(ioc));
    return G_SOURCE_REMOVE;
}

void fd_start_incoming_migration(const char *infd, Error **errp)
{
    QIOChannel *ioc;
    int fd;

    fd = strtol(infd, NULL, 0);
    trace_migration_fd_incoming(fd);

    ioc = qio_channel_new_fd(fd, errp);
    if (!ioc) {
        close(fd);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "migration-fd-incoming");
    qio_channel_add_watch_full(ioc, G_IO_IN,
                               fd_accept_incoming_migration,
                               NULL, NULL,
                               g_main_context_get_thread_default());
}

