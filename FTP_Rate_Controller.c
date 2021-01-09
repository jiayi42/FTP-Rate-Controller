/**
 * Simple FTP Proxy For Introduction to Computer Network.
 * Author: z58085111 @ HSNL
 * **/
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#define MAXSIZE 2048
#define FTP_PORT 8740
#define FTP_PASV_CODE 227
#define FTP_STOR_COMD "STOR"
#define FTP_ADDR "140.114.71.159"
#define max(X,Y) ((X) > (Y) ? (X) : (Y))

int proxy_IP[4];
struct token {//create a token data type
    pthread_mutex_t mutex;//Mutual exclusion
    int count;
    int count_loop;//the total transmit flag
    int rate;
    int token_per_time;
    bool download;//download or upload
    double t; // delay time for generating/consuming token
};
int connect_FTP(int ser_port, int clifd);
int proxy_func(int ser_port, int clifd, int rate, bool download, int count_loop);
int create_server(int port);
void *token_generator(void *data);
int rate_control(struct token * proxy_token, int fd, char* buffer, int byte_num, int rate, int count_loop, bool download);


int main (int argc, char **argv) {
    int ctrlfd, connfd, port, rate;
    pid_t childpid;
    socklen_t clilen;
    struct sockaddr_in cliaddr;
    if (argc < 4) {
        printf("[v] Usage: ./executableFile <ProxyIP> <ProxyPort> <Rate>\n");
        return -1;
    }

    sscanf(argv[1], " %d.%d.%d.%d", &proxy_IP[0], &proxy_IP[1], &proxy_IP[2], &proxy_IP[3]);
    port = atoi(argv[2]);
    rate = atoi(argv[3]);
    //low speed needs to be record and fix bias via experience
    if (rate > 20 && rate <= 30) {
	rate=28;
    }
    if (rate > 30 && rate <= 40) {
	rate=35;
    }
    if (rate > 48 && rate <= 55) {
	rate=55;
    }
    ctrlfd = create_server(port);
    clilen = sizeof(struct sockaddr_in);
    for (;;) {
        connfd = accept(ctrlfd, (struct sockaddr *)&cliaddr, &clilen);
        if (connfd < 0) {
            printf("[x] Accept failed\n");
            return 0;
        }

        printf("[v] Client: %s:%d connect!\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
        if ((childpid = fork()) == 0) {
            close(ctrlfd);
            proxy_func(FTP_PORT, connfd, rate, false, 0);
            close(connfd);
            printf("[v] Client: %s:%d terminated!\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
            exit(0);
        }

        close(connfd);
    }
    return 0;
}

int connect_FTP(int ser_port, int clifd) {
    int sockfd;
    char addr[] = FTP_ADDR;
    int byte_num;
    char buffer[MAXSIZE];
    struct sockaddr_in servaddr;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("[x] Create socket error");
        return -1;
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(ser_port);

    if (inet_pton(AF_INET, addr, &servaddr.sin_addr) <= 0) {
        printf("[v] Inet_pton error for %s", addr);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        printf("[x] Connect error");
        return -1;
    }

    printf("[v] Connect to FTP server\n");
    if (ser_port == FTP_PORT) {
        if ((byte_num = read(sockfd, buffer, MAXSIZE)) <= 0) {
            printf("[x] Connection establish failed.\n");
        }

        if (write(clifd, buffer, byte_num) < 0) {
            printf("[x] Write to client failed.\n");
            return -1;
        }
    }

    return sockfd;
}

int proxy_func(int ser_port, int clifd, int rate,bool download, int count_loop) {
    char buffer[MAXSIZE];
    int serfd = -1, datafd = -1, connfd;
    int data_port;
    int byte_num;
    int status, pasv[7];
    int childpid;
    int balance_factor=100;
    //int count_loop
    int rate_worker=-1;
    socklen_t clilen;
    struct sockaddr_in cliaddr;

    // select the variables
    int maxfdp1;
    int i, nready = 0;
    fd_set rset, allset;

    // connect to FTP server
    if ((serfd = connect_FTP(ser_port, clifd)) < 0) {
        printf("[x] Connect to FTP server failed.\n");
        return -1;
    }


    // initialize select the variables
    FD_ZERO(&allset);
    FD_SET(clifd, &allset);
    FD_SET(serfd, &allset);
    // tokens gets the messages of upload and download and count_loop by initializing
    struct token *proxy_token = (struct token *)malloc(sizeof(struct token));
    proxy_token->rate = rate*1024;
    proxy_token->count = 0;
    proxy_token->count_loop = count_loop;
    proxy_token->download = download;
    // to balance the speed error between consume and generate tokens
    int setval =  proxy_token->rate;
    if (proxy_token->rate >= (MAXSIZE*balance_factor)){
        proxy_token->token_per_time =MAXSIZE;
        setval =MAXSIZE;
    }
    else{
        proxy_token->token_per_time =  (proxy_token->rate/balance_factor);
    }
    proxy_token->t = ((double)(proxy_token->token_per_time)/proxy_token->rate);

    // start a pthread to generate token
    pthread_mutex_init(&proxy_token->mutex,NULL);
    pthread_t pthread_id;
    pthread_attr_t attribute;
    pthread_attr_init(&attribute);
    pthread_create(&pthread_id, &attribute, token_generator, (void*)proxy_token);

    setsockopt(clifd,SOL_SOCKET,SO_RCVBUF,(char *)&setval, sizeof(int));
    setsockopt(serfd,SOL_SOCKET,SO_RCVBUF,(char *)&setval, sizeof(int));

    // selecting
    for (;;) {
	count_loop=count_loop+1;
        // reset the select variables
        rset = allset;
        maxfdp1 = max(clifd, serfd) + 1;


        nready = select(maxfdp1, &rset, NULL, NULL, NULL);// select descriptor,I/O multiplexer
        if (nready > 0) {//something can be read/written, if nready = 0, means nothing can be read/written.


            // check FTP client socket fd and upload
            if (FD_ISSET(clifd, &rset)) {//test if clifd is in the rset.
                memset(buffer, '\0', MAXSIZE);
                // read from TCP recv buffer
                byte_num = read(clifd, buffer, proxy_token->token_per_time);
                // observe that proxy receiving byte_num from client to upload
                if (byte_num<= 0) {
                    printf("[!] Client terminated the connection.\n");
                    break;
                }

                // client blocking write to TCP send buffer from proxy to server before proxy_token->count reaching byte_num 
                rate_worker=rate_control(proxy_token, serfd, buffer, byte_num, rate, count_loop, false);
                if (rate_worker != 0) {
                    printf("[x] Write fail server in rate controling write\n");
                    break;
                }
            }

            // check FTP server socket fd and download
            if (FD_ISSET(serfd, &rset)) {
                memset(buffer, '\0', MAXSIZE);
                // observe that proxy receiving byte_num from server to download
                if ((byte_num = read(serfd, buffer, proxy_token->token_per_time)) <= 0) {
                    printf("[!] Server terminated the connection.\n");
                    break;
                }

                if(ser_port == FTP_PORT) {
                    buffer[byte_num] = '\0';
                    status = atoi(buffer);
                    if (status == FTP_PASV_CODE && ser_port == FTP_PORT) {

                        sscanf(buffer, "%d Entering Passive Mode (%d,%d,%d,%d,%d,%d)",&pasv[0],&pasv[1],&pasv[2],&pasv[3],&pasv[4],&pasv[5],&pasv[6]);
                        memset(buffer, '\0', MAXSIZE);
                        // force data connection to proxy, set to issue1
                        sprintf(buffer, "%d Entering Passive Mode (%d,%d,%d,%d,%d,%d)\n", status, proxy_IP[0], proxy_IP[1], proxy_IP[2], proxy_IP[3], pasv[5], pasv[6]);

                        if ((childpid = fork()) == 0) {
                            data_port = pasv[5] * 256 + pasv[6];
                            datafd = create_server(data_port);
                            if (write(clifd, buffer, byte_num) < 0) {
                                printf("[x] Write to client failed.\n");
                                break;
                            }
                            printf("[-] Waiting for data connection!\n");
                            clilen = sizeof(struct sockaddr_in);
                            connfd = accept(datafd, (struct sockaddr *)&cliaddr, &clilen);
                            if (connfd < 0) {
                                printf("[x] Accept failed\n");
                                return 0;
                            }

                            printf("[v] Data connection from: %s:%d connect.\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
                            #vswitch to download mode
                            proxy_func(data_port, connfd, rate, true, count_loop);
                            close(connfd);
                            printf("[!] End of data connection!\n");
                            exit(0);
                        }
                    }
                    else {
                                if (write(clifd, buffer, byte_num) < 0) {
                                    printf("[x] Write to client failed.\n");
                                    break;
                                }
                    }
                } 
                else {
                    #this code section happens when switch to download mode and condition if (ser_port == FTP_PORT) has been check before switching
                    #thus the new proxy_func call after switch will natually check out with false of ser_port == FTP_PORT (now it is data_port)
                    # client blocking write to TCP rev buffer from proxy to client before proxy_token->count reaching byte_num
                    rate_worker=rate_control(proxy_token, clifd, buffer, byte_num, rate, count_loop, true);
                    if (rate_worker != 0) {
                        printf("[x] Download fail in rate controling write\n");
                        break;
                    }
                }
            }
        } else {
            printf("[x] Select() returns -1. ERROR!\n");
            return -1;
        }
    }
    // cancle token generator
    if (pthread_cancel(pthread_id) == 0) {
        pthread_mutex_destroy(&proxy_token->mutex);
        free(proxy_token);
    }
    return 0;
}

int create_server(int port) {
    int listenfd;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&servaddr , sizeof(servaddr)) < 0) {
        //print the error message
        perror("bind failed. Error");
        return -1;
    }

    listen(listenfd, 3);
    return listenfd;
}

void *token_generator(void *data) {
    // detach thread from process
    pthread_detach(pthread_self());
    struct token* proxy_token;
    proxy_token = (struct token*)data;
    // avoid block by thread lock
    clock_t start, end;
    double dur = 0;
    int token_per_time = proxy_token->token_per_time;
    int count_loop = proxy_token->count_loop;
    bool download=proxy_token->download;
    int usleep_time = (int)1000000*(proxy_token->t);
    if (download==true){//initialize download slower
	    if ( count_loop>1000 && count_loop<=6000){//initialize download slower
		    usleep_time = (int)1300000*(proxy_token->t);
	    }
    }
    if (download==false){//initialize upload faster
	    if ( count_loop>0 && count_loop<=2000){//initialize upload faster
		    usleep_time = (int)1100000*(proxy_token->t);
	    }
	    if ( count_loop>2000 && count_loop<=5000){//initialize upload faster faster
		    usleep_time = (int)500000*(proxy_token->t);
	    }
	    if ( count_loop>5000 && count_loop<=7000){//initialize upload faster faster faster
		    usleep_time = (int)10000*(proxy_token->t);
	    }
    }

    while (1) {
        // delay
        usleep(usleep_time);
        // dur can not count sleep time into it
        start = clock();
        pthread_testcancel(); // cancle point
        pthread_mutex_lock(&proxy_token->mutex);
        if (proxy_token->count < 2*token_per_time) {
            proxy_token->count += token_per_time + (int)(dur * proxy_token->rate);
            //printf("generate %d tokens\n", proxy_token->count);
        }
        // unlock
        pthread_mutex_unlock(&proxy_token->mutex);
        pthread_testcancel();
        end = clock();
        dur = (double)(end-start)/CLOCKS_PER_SEC;
    }
    return ((void*)0);
}
int rate_control(struct token* proxy_token, int fd, char* buffer, int byte_num, int rate, int count_loop, bool download) {
    // seperate packet to maintain fluent data flow
    int usleep_time = (int)1000000*(proxy_token->t);
    if (download==true){//initialize download slower
	    if ( count_loop>1000 && count_loop<=4000){//initialize download slower
		    usleep_time = (int)1300000*(proxy_token->t);
	    }
    }

    if (download==false){//initialize upload faster
	    if ( count_loop>0 && count_loop<=2000){//initialize upload faster
		    usleep_time = (int)1100000*(proxy_token->t);
	    }
	    if ( count_loop>2000 && count_loop<=5000){//initialize upload faster faster
		    usleep_time = (int)500000*(proxy_token->t);
	    }
	    if ( count_loop>5000 && count_loop<=7000){//initialize upload faster faster faster
		    usleep_time = (int)10000*(proxy_token->t);
	    }
    }

    //pthread_mutex_lock helps the protection of proxy_token->count for the while loop judge 
    pthread_mutex_lock(&proxy_token->mutex);
    while (proxy_token->count < byte_num) {
        pthread_mutex_unlock(&proxy_token->mutex);
        usleep(usleep_time);
        pthread_mutex_lock(&proxy_token->mutex);
    }
    //printf("consume %d tokens %d\n", proxy_token->count,byte_num);
    proxy_token->count -= byte_num;
    pthread_mutex_unlock(&proxy_token->mutex);

    // write
    if (write(fd, buffer, byte_num) < 0) {
        printf("[x] Write to server failed.\n");
        return -1;
    }
    return 0;
}
