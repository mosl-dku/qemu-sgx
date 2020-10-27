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
#include <pthread.h>

void fd_start_outgoing_migration(MigrationState *s, const char *fdname, Error **errp)
{
    QIOChannel *ioc;
    struct sockaddr_in servaddr; // QUOTE
    int sock;
    char buf[256];
    char filename[20];
    int fp, filesize;
    int sread, total = 0;

    bool result = false;
    bool verf_result;
    int thr_id;
    pthread_t p_thread;

    while(!result){
        printf("LOG : TRY receive finished\n");
        result = file_recv();
        if(!result){
            printf("LOG : recv failed...\n");
	    return;
        }
    }

    if(access("/tmp/quote.dat", F_OK ) != -1) {
        // make thread for wait verification result
        thr_id = pthread_create(&p_thread, NULL, (void*)msg_recv, &verf_result);
        if (thr_id < 0){
            printf("LOG : msg-recv: thread create error\n");
                return;
        }
        // send quote to DCAP Server
        sock = socket(PF_INET, SOCK_STREAM, 0);
        if(sock < 0){
            perror("socket fail");
            return;
        }
        bzero((char*)&servaddr, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = inet_addr("172.25.244.76");
        servaddr.sin_port = htons(5500);

        int connect_res = connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr));
        
        if(connect_res < 0){
                perror("LOG : quote2dcap : connect fail\n");
                return;
            }
        fp = open("/tmp/quote.dat", O_RDONLY);
        if(fp < 0){
            printf("LOG : quote2dcap : open fail \n");
            return;
        }
        strcpy(filename, "quote.dat");
        send(sock, filename, sizeof(filename), 0);

        filesize = lseek(fp, 0, SEEK_END);
        send(sock, &filesize, sizeof(filesize), 0);
        lseek(fp, 0, SEEK_SET);

        while(total != filesize){
	       	sread = read(fp, buf, 100);
	        total += sread;
	        buf[sread] = 0;
	        send(sock, buf, sread, 0);
	        usleep(10000);
	    }

        printf("LOG : quote2dcap : quote send complete\n");
        close(fp);
        close(sock);

        if(access("/tmp/quote.dat", F_OK ) != -1) {
            if(system("rm /tmp/quote.dat") != -1)
                printf("LOG : quote2dcap : remove quote.dat\n");
        }else{
            printf("LOG : quote2dcap : can't find recv data to remove\n");
        }
        pthread_join(p_thread, NULL);
        printf("LOG : msg recv finish\n");
	}else{
		printf("ERR : File recv failed\n");
        return;
	}

    // check quote verification result
	if(!verf_result){
		return;
    }

    // go back to original migration routine
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

void msg_recv(bool *quote_verf){
    int server_sockfd, client_sockfd;
    int state;
    socklen_t client_len;
    struct stat;
    struct sockaddr_in clientaddr, serveraddr;

    char buf[10];
    memset(buf, 0x00, 10);
    state = 0;

    client_len = sizeof(clientaddr);
    bzero((char*)&serveraddr, sizeof(serveraddr));
    //memset(&serveraddr,0x00, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(9999);

    client_len = sizeof(clientaddr);

    if ((server_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        printf("ERR : msg-recv: socket error - thread exit\n");
    }

    state = bind(server_sockfd , (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (state == -1) {
        printf("ERR : msg-recv: bind error - thread exit\n");
    }

    state = listen(server_sockfd, 5);
    if (state == -1) {
        printf("ERR : msg-recv: listen error - thread exit\n");
    }

    client_sockfd = accept(server_sockfd, (struct sockaddr *)&clientaddr, &client_len);

    recv(client_sockfd, buf, sizeof(buf), 0);

    if(strcmp(buf, "1")==0){
        printf("LOG : msg-recv: Quote Verification Success");
        *quote_verf = true;
    }else{
        printf("ERR : msg-recv: Quote Verification Failed\n");
        *quote_verf = false;
    }

    close(client_sockfd);
    close(server_sockfd);
    pthread_exit(NULL);
}

bool file_recv(void){
    int client_len;
    int client_sockfd;
    char buf[256];
    struct sockaddr_in clientaddr;

    char addr[20];
    printf("LOG : file-recv: MIG : file_recv\n");
    FILE *fp = fopen("/tmp/mig_tgtip.tmp", "r");
    if(fgets(addr, sizeof(addr), fp)!= NULL)
        printf("LOG : file-recv: ip addr = %s\n", addr);
    fclose(fp);

    clientaddr.sin_family = AF_INET;
    clientaddr.sin_addr.s_addr = inet_addr(addr);
    clientaddr.sin_port = htons(8888);
    client_len = sizeof(clientaddr);

    client_sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (connect (client_sockfd, (struct sockaddr *)&clientaddr, client_len) < 0){
        printf("ERR : file-recv: connect error\n");
        return false;
    }

    int nbyte;
    size_t filesize = 0, bufsize = 0;
    FILE *file = NULL;

    file = fopen("/tmp/quote.dat", "wb");

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

