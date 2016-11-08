#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#define MAX_HEADER_SIZE (8192)
#define BUF_SIZE (8192)
#define MAX_HOST_SIZE (128)
#define MAX_PATH_SIZE (128)

int localPort;
int serverPort;
char serverHost[MAX_HOST_SIZE];

char accessDir[MAX_PATH_SIZE];
char logDir[MAX_PATH_SIZE];

void wblog(char *s) {
    if (!strlen(logDir)) {
        printf("no log dir\n");
        return;
    }

    char day[128];
    char daytime[128];
    time_t now;
    struct tm *ti;
    
    time(&now);
    ti = localtime(&now);
    strftime(day, 128, "%Y-%m-%d", ti);
    strftime(daytime, 128, "%Y-%m-%d %H:%M:%S", ti);

    strcat(logDir, day);
    strcat(logDir, ".log");

    FILE *f = fopen(logDir, "ab+");
    if (!f) {
        printf("open log file error\n");
        return;
    }

    fprintf(f, "[%s]%s\n", daytime, s);
    printf("[%s]%s\n", daytime, s);
    fclose(f);
}

void wblogf(char *format, ...) {
    char buf[1024];
    memset(&buf, 0, 256);

    va_list args;
    va_start(args, format);
    vsnprintf(buf, 1024, format, args);
    va_end(args);

    wblog(buf);
}

void transpond(int fromSd, int toSd) {
    char buf[BUF_SIZE];
    int cnt;
    while (1) {
        cnt = recv(fromSd, buf, BUF_SIZE, 0);
        if (cnt > 0) {
            printf("from %d recv:%s\n", fromSd, buf);
            send(toSd, buf, cnt, 0);
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

        if (isTunnel) {
            *port = 443; 
        } else {
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
    printf("extract host ok %s %d %d\n", host, *port, *isTunnel);
}

int createConnection(char *host, int port) {
    if (!host || !port) {
        return -1;
    }

    struct hostent *ht = gethostbyname(host);
    if (!ht) {
        printf("gethostbyname error\n");
        exit(-1);
    }

    int sd;
    struct sockaddr_in addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, ht->h_addr_list[0], ht->h_length);

    if (connect(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
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
            printf("readHeader outof size\n");
            exit(-1);
        }
        
        cnt = recv(sd, &ch, 1, 0);
        if (cnt > 0) {
            *p = ch;

            if (ch == '\n' && (((p - header) > 1 && *(p - 1) == '\n') || ((p - header) > 2 && *(p - 2) == '\n'))) {
                 break;
            }
            pCh = ch;
            p++;
        } else {
            if (EINTR == cnt || EWOULDBLOCK == cnt || EAGAIN == cnt) {
                continue;
            }
            printf("readHeader %d\n", cnt);
            exit(-1);
        }
    }
    printf("readHeader ok:\n%s\n", header);
}

void handleClient(int clientSd, struct sockaddr_in addr) {
    printf("client: %s, %d\n", inet_ntoa(addr.sin_addr), addr.sin_port); 

    printf("create connection to server\n");
    int isTunnel;
    char header[MAX_HEADER_SIZE];
    int serverSd;
    if (serverPort) {
        serverSd = createConnection(serverHost, serverPort);
        
        if (serverSd < 0) {
            printf("create error\n");   
            exit(-1);
        }
    } else {
        readHeader(clientSd, header, MAX_HEADER_SIZE);

        char host[MAX_HOST_SIZE];
        int port;
        extractHostPort(header, host, &port, &isTunnel);
        
        serverSd = createConnection(host, port);

        if (serverSd < 0) {
            printf("create error\n");   
            exit(-1);
        }
    }

    if (isTunnel) {
        printf("send tunnel established\n");
        char *respond = "HTTP/1.1 200 Connection Established\r\n\r\n";
        int cnt;
        if ((cnt = send(clientSd, respond, strlen(respond), 0)) < 0) {
            printf("send tunnel establish error\n");
            exit(-1);   
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        exit(-1);
    } else if (0 == pid) {
        if (strlen(header) && !isTunnel) {
            printf("send header\n");
            send(serverSd, header, strlen(header), 0);
        }
        printf("child transpond c to s\n");
        transpond(clientSd, serverSd);   
        printf("client socket closed, kill parent process to close server socket\n");
        kill(getppid(), SIGKILL);
        exit(0);
    } else {
        printf("parent transpond s to c\n");
        transpond(serverSd, clientSd);
        printf("server socket closed, kill child process to close client socket\n");
        kill(pid, SIGKILL);
    }
}

void start() {
    int sd;
    struct sockaddr_in addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        printf("socket() error\n");
        exit(1);
    }

    int opt;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        printf("setsockopt error\n");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("bind error\n");
        exit(1);
    }

    if (listen(sd, 20) != 0) {
        printf("listen error\n");
        exit(1);
    }
    
    printf("server started\n");
    int len;
    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen;
        int clientSd = accept(sd, (struct sockaddr*)&clientAddr, &clientAddrLen);

        pid_t pid = fork();
        if (pid < 0) {
            printf("fork error\n");
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
    printf(" -l log directory\n");
}

int main(int argc, char *argv[]) {
	int opt;
    bool daemon = false;
    char *ch = NULL;
	while ((opt = getopt(argc, argv, ":dp:h:l:")) != -1) {
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
            case 'l':
                memset(&logDir, 0, MAX_PATH_SIZE);
                strcpy(logDir, optarg);
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
        pid_t pid = fork();
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
