#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "debug_fork.h"
#include "wblog.h"

typedef enum endirection {
    to,
    from
} endirection;

#define MAX_HEADER_SIZE (8192)
#define BUF_SIZE (8192)
#define MAX_HOST_SIZE (128)
#define MAX_PATH_SIZE (128)

char enKey[] = "h";

int localPort;
int serverPort;
char serverHost[MAX_HOST_SIZE];
int isCapture;
char capturePath[MAX_PATH_SIZE];

int isEncrypt;
endirection endir;


void printBits(size_t const size, void const * const ptr)
{
    unsigned char *b = (unsigned char*) ptr;
        unsigned char byte;
            int i, j;

                for (i=size-1;i>=0;i--)
                    {
                            for (j=7;j>=0;j--)
                                    {
                                                byte = (b[i] >> j) & 1;
                                                            printf("%u", byte);
                                                                    }
                                                                        }
                                                                            puts("");
                                                                            }
void wbxor(const void *msg, size_t len, void *dst, size_t dstLen, const char *key) {
    int keyLen = strlen(key);
    int i;
    int minLen = len - dstLen > 0 ? dstLen : len;
    for (i = 0; i < minLen; i++) {
        char ch = key[i % keyLen];
        ((char *)dst)[i] = ((char *)msg)[i] ^ ch;
    }
}

ssize_t wbsend(int socket, const void *buffer, size_t length, int flags) {
    if (isEncrypt && to == endir) {
        void *p = malloc(length);
        wbxor(buffer, length, p, length, enKey);
        int cnt = send(socket, p, length, flags);
        free(p);
        return cnt;
    } else {
        return send(socket, buffer, length, flags);
    }
}

ssize_t wbrecv(int socket, void *buffer, size_t length, int flags) {
    if (isEncrypt && from == endir) {
        int cnt;
        cnt = recv(socket, buffer, length, flags);       

        void *p = malloc(cnt);
        wbxor(buffer, cnt, p, cnt, enKey);

        memset(buffer, 0, cnt);
        memcpy(buffer, p, cnt);
        free(p);
        return cnt;
    } else {
        return recv(socket, buffer, length, flags);
    }
}


void capture(char *format, ...) {
    if (!isCapture && !strlen(capturePath)) return;

    char buf[BUF_SIZE];
    memset(buf, 0, BUF_SIZE);

    va_list args;
    va_start(args, format);
    vsnprintf(buf, BUF_SIZE, format, args);
    va_end(args);

    if (strlen(capturePath)) {
        FILE *f = fopen(capturePath, "a+");
        if (!f) {
            printf("open capture file error\n");
            return;
        }
        fprintf(f, "%s", buf);
        fflush(f);
        fclose(f);
    } else {
        printf("\n[CAPTURE START]\n");
        printf("%s", buf);
        printf("[CAPTURE END]\n");
    }
}

void transpond(int fromSd, int toSd) {
    char buf[BUF_SIZE];
    int cnt;
    while (1) {
        memset(buf, 0, BUF_SIZE);
        cnt = wbrecv(fromSd, buf, BUF_SIZE, 0);
        if (cnt > 0) {
            wbsend(toSd, buf, cnt, 0);
            buf[cnt] = '\0';
            wblogf("from %d recv: cnt=%d %s", fromSd,  cnt, buf);
            capture(buf);
        } else {
            if (EINTR == cnt || EWOULDBLOCK == cnt || EAGAIN == cnt) {
                continue;
            }
            break;
        }
    }
}

void extractHostPort(char *header, char *host, int *port, int *isTunnel) {
    if (!header) {
        exit(-1);
    }

    char *tunnel = strstr(header, "CONNECT");
    *isTunnel = tunnel != NULL ? 1 : 0;

    char *start = strstr(header, "Host:");
    if (!start) {
        exit(-1);
    }
    start += 5; //Host:
    start ++; //0x20

    char *end = strchr(start, '\r');
    if (!end) {
        end = strchr(start, '\n');
        if (!end) {
            exit(-1);
        }
    }

    char *colon = strchr(start, ':');
    if (!colon || colon > end) {
        int len = end - start;
        strncpy(host, start, len);
        host[len] = '\0';

        wblogf("===isTunnel=%d", *isTunnel);
        if (*isTunnel) {
            wblogf("===443===");
            *port = 443; 
        } else {
            wblogf("===80===");
            *port = 80;
        }
    } else {
        int len = colon - start;
        strncpy(host, start, len);
        host[len] = '\0';

        colon++;
        len = end - colon + 1;
        char tmpPort[len];
        strncpy(tmpPort, colon, len);
        tmpPort[len] = '\0';
        *port = atoi(tmpPort);
    }
    wblogf("extract host ok %s %d %d", host, *port, *isTunnel);
}

int createConnection(char *host, int port) {
    if (!host || !port) {
        return -1;
    }

    struct hostent *ht = gethostbyname(host);
    if (!ht) {
        wblogf("gethostbyname error");
        exit(-1);
    }

    int sd;
    struct sockaddr_in addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        wblogf("create connection error socket():%d", sd);
        return sd;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, ht->h_addr_list[0], ht->h_length);

    if (connect(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        wblogf("create connection error connect():%d", errno);
        return -1;
    }

    return sd;
}

void readHeader(int sd, char *header, int size) {
    char ch, pCh;
    int cnt;
    char *p = header;
    while (1) {
        if (p - header > size) {
            wblogf("readHeader outof size");
            exit(-1);
        }
        
        cnt = wbrecv(sd, &ch, 1, 0);
        if (cnt > 0) {
            *p = ch;

            if (ch == '\n' && (((p - header) > 1 && *(p - 1) == '\n') || ((p - header) > 2 && *(p - 2) == '\n'))) {
                 break;
            }
            pCh = ch;
            p++;
        } else {
            if (EINTR == cnt || EWOULDBLOCK == cnt || EAGAIN == cnt) {
                wblogf("???????");
                continue;
            }
            wblogf("readHeader %d", cnt);
            exit(-1);
        }
    }
    wblogf("readHeader ok:\n%s", header);
}

void handleClient(int clientSd, struct sockaddr_in addr) {
    wblogf("client:%d, %s, %d", clientSd, inet_ntoa(addr.sin_addr), addr.sin_port);

    if (serverPort) {
        endir = to;
    } else {
        endir = from;
    }

    int isTunnel;
    char header[MAX_HEADER_SIZE];
    memset(header, 0, MAX_HEADER_SIZE);
    int serverSd;
    if (serverPort) {
        wblogf("create connection to my server: %s %d", serverHost, serverPort);
        serverSd = createConnection(serverHost, serverPort);
        
        if (serverSd < 0) {
            exit(-1);
        }
    } else {
        wblogf("begin read header");
        readHeader(clientSd, header, MAX_HEADER_SIZE);

        char host[MAX_HOST_SIZE];
        int port;
        extractHostPort(header, host, &port, &isTunnel);
        
        wblogf("create connection to web server: %s %d", host, port);
        serverSd = createConnection(host, port);

        if (serverSd < 0) {
            exit(-1);
        }
    }
    wblogf("created server connection:%d", serverSd);

    pid_t pid = debug_fork();
    if (pid < 0) {
        exit(-1);
    } else if (0 == pid) {
        if (serverPort) {
            endir = from;
        } else {
            endir = to;
        }

        if (isTunnel) {
            wblogf("send tunnel established");
            char *respond = "HTTP/1.1 200 Connection Established\r\n\r\n";
            int cnt;
            if ((cnt = wbsend(clientSd, respond, strlen(respond), 0)) < 0) {
                wblogf("send tunnel establish error");
                exit(-1);   
            }
        }
        
        wblogf("transpond s to c");
        transpond(serverSd, clientSd);
        wblogf("server socket closed, kill parent process to close client socket");
        kill(getppid(), SIGKILL);
        exit(0);
    } else {
        if (strlen(header) && !isTunnel) {
            wblogf("send header:%s", header);
            capture(header);
            wbsend(serverSd, header, strlen(header), 0);
        }
        wblogf("transpond c to s");
        transpond(clientSd, serverSd);   
        wblogf("client socket closed, kill child process to close server socket");
        kill(pid, SIGKILL);
    }
}

void start() {
    int sd;
    struct sockaddr_in addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        wblogf("socket() error");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        wblogf("setsockopt error");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        wblogf("bind error");
        exit(1);
    }

    if (listen(sd, 20) != 0) {
        wblogf("listen error");
        exit(1);
    }
    
    wblogf("server started");
    int len;
    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen;
        int clientSd = accept(sd, (struct sockaddr*)&clientAddr, &clientAddrLen);

        pid_t pid = debug_fork();
        if (pid < 0) {
            wblogf("fork error");
            exit(1);
        } else if (0 == pid) {
            close(sd);
            handleClient(clientSd, clientAddr);
            exit(0);
        } else {
            close(clientSd);
        }
    }
}

void sigchld_handler() {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void usage() {
    printf("Usage:\n");
    printf(" -d run as daemon\n");
    printf(" -p local port\n");
    printf(" -h remote host and port, default port is 80, such as example.com:9000\n");
    printf(" -c capture http data\n");
    printf(" -w capture redirect to file\n");
    printf(" -e enable encode/decode\n");
}

int main(int argc, char *argv[]) {
	int opt;
    bool daemon = false;
    char *ch = NULL;
	while ((opt = getopt(argc, argv, ":dcew:p:h:l:")) != -1) {
		switch (opt) {
			case 'd':
                daemon = true;
				break;
			case 'p':
                localPort = atoi(optarg);
                break;
            case 'h':
                ch = strchr(optarg, ':');
                if (ch) {
                    strncpy(serverHost, optarg, ch - optarg);
                    serverPort = atoi(ch + 1);
                } else {
                    strcpy(serverHost, optarg);
                    serverPort = 80;
                }
                break;
            case 'c':
                isCapture = 1;
                break;
            case 'w':
                memset(capturePath, 0, MAX_PATH_SIZE);
                strcpy(capturePath, optarg);
                break;
            case 'e':
                isEncrypt = 1;
                break;
                
            default:
                usage();
                exit(1);
		}
	}

    if (!localPort) {
        printf("please appoint local port\n\n");
        usage();
        exit(1);
    }

    //signal for child process
    signal(SIGCHLD, sigchld_handler);   

    if (daemon) {
        pid_t pid = debug_fork();
        if (pid < 0) {
            printf("Could not daemon!\n");
            exit(1);
        } else if (0 == pid) {
            start();
        }
    } else {
        start();
    }
}
