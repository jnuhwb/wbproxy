#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "debug_fork.h"
#include "wblog.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define EWOULDBLOCK WSAEWOULDBLOCK
#define SIGKILL -1
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#import <execinfo.h>

#define SOCKET_ERROR -1
#endif

#define MAX_HEADER_SIZE (8192)
#define BUF_SIZE (8192)
#define MAX_HOST_SIZE (128)
#define MAX_PATH_SIZE (128)

typedef struct Option
{
	int daemon;
	int localPort;
	int serverPort;
	char serverHost[MAX_HOST_SIZE];
	bool isCapture;
	char capturePath[MAX_PATH_SIZE];
	bool isEncrypt;
} Option;

typedef struct AcceptTreadParam
{
	struct sockaddr_in addr;
	int sd;	
} AcceptThreadParam;

typedef struct TranspondThreadParam
{
	int clientSd;
	int serverSd;
} TranspondThreadParam;

typedef enum endirection {
    To,
    From
} endirection;

//global
char enKey[] = "h";
Option myopt;	
endirection endir;

void closeSocket(int sd)
{
	wblogf("close socket: %d", sd);
#ifdef WIN32
	closesocket(sd);
#else
	close(sd);
#endif
}

void printBits(size_t const size, void const *const ptr) {
    unsigned char *b = (unsigned char *) ptr;
    unsigned char byte;
    int i, j;

    for (i = size - 1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
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

ssize_t wbsend(int socket, const void *buffer, size_t length, int flags, bool en) {
    if (myopt.isEncrypt && en) {
        void *p = malloc(length);
        wbxor(buffer, length, p, length, enKey);
        int cnt = send(socket, p, length, flags);
        free(p);
        return cnt;
    } else {
        return send(socket, buffer, length, flags);
    }
}

ssize_t wbrecv(int socket, void *buffer, size_t length, int flags, bool de) {
    if (myopt.isEncrypt && de) {
        int cnt;
        cnt = recv(socket, buffer, length, flags);       
		if (cnt > 0) {
				void *p = malloc(cnt);
				wbxor(buffer, cnt, p, cnt, enKey);

				memset(buffer, 0, cnt);
				memcpy(buffer, p, cnt);
				free(p);
		}

        return cnt;
    } else {
        return recv(socket, buffer, length, flags);
    }
}


void capture(char *format, ...) {
    if (!myopt.isCapture && !strlen(myopt.capturePath)) return;

    char buf[BUF_SIZE];
    memset(buf, 0, BUF_SIZE);

    va_list args;
    va_start(args, format);
    vsnprintf(buf, BUF_SIZE, format, args);
    va_end(args);

    if (strlen(myopt.capturePath)) {
        FILE *f = fopen(myopt.capturePath, "a+");
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

void transpond(int fromSd, int toSd, bool enSend) {
	wblogf("transpond from %d to %d", fromSd, toSd);
    char buf[BUF_SIZE];
    int cnt;
    while (1) {
        memset(buf, 0, BUF_SIZE);
        cnt = wbrecv(fromSd, buf, BUF_SIZE, 0, !enSend);
        if (cnt > 0) {
            wblogf("from %d recv: cnt=%d", fromSd,  cnt);
            ssize_t scnt = wbsend(toSd, buf, cnt, 0, enSend);
            wblogf("send to %d: cnt=%d", toSd,  scnt);
        } else {
            if ((cnt < 0) && (EINTR == errno || EWOULDBLOCK == errno || EAGAIN == errno)) {
                continue;
            }
            if (cnt < 0) {
                wbloglf(LogLevelError, "error: %d transpond from %d to %d", errno, fromSd, toSd);
            }
            break;
        }
    }
}

int extractHostPort(char *header, char *host, int *port, int *isTunnel) {
    if (!header) {
		return -1;
    }

    char *tunnel = strstr(header, "CONNECT");
    *isTunnel = tunnel != NULL ? 1 : 0;

    char *start = strstr(header, "Host:");
    if (!start) {
		return -1;
    }
    start += 5; //Host:
    start ++; //0x20

    char *end = strchr(start, '\r');
    if (!end) {
        end = strchr(start, '\n');
        if (!end) {
			return -1;
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
	return 0;
}

int createConnection(char *host, int port) {
    if (!host || !port) {
        return -1;
    }

    struct hostent *ht = gethostbyname(host);
    if (!ht) {
        wbloglf(LogLevelError, "gethostbyname error");
        return -1;
    }

    int sd;
    struct sockaddr_in addr;

    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        wbloglf(LogLevelError, "create connection error socket():%d", sd);
        return sd;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, ht->h_addr_list[0], ht->h_length);

    if (connect(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        wbloglf(LogLevelError, "create connection error connect():%d", errno);
        return -1;
    }

    return sd;
}

int readHeader(int sd, char *header, int size) {
    char ch, pCh;
    int cnt;
    char *p = header;
    while (1) {
        if (p - header > size) {
            wbloglf(LogLevelError, "readHeader outof size");
			return -1;
        }
        
        cnt = wbrecv(sd, &ch, 1, 0, myopt.isEncrypt);
        if (cnt > 0) {
            *p = ch;

            if (ch == '\n' && (((p - header) > 1 && *(p - 1) == '\n') || ((p - header) > 2 && *(p - 2) == '\n'))) {
				return 0;
            }
            pCh = ch;
            p++;
        } else {
            if ((cnt < 0) && (EINTR == errno || EWOULDBLOCK == errno || EAGAIN == errno)) {
                continue;
            }
            if (cnt < 0) {
                wbloglf(LogLevelError, "readHeader error %d", errno);
            } else {
                wblogf("readHeader socket %d closed", sd);
            }
			return -1;
        }
    }
}

#ifdef WIN32
DWORD WINAPI transpondThread(LPVOID lpParam)
#else
void *transpondThread(void *lpParam)
#endif
{
	TranspondThreadParam *p = (TranspondThreadParam *)lpParam;
	transpond(p->serverSd, p->clientSd, myopt.serverPort ? false : true);
	free(p);
    return 0;
}

void dualTranspond(int clientSd, int serverSd)
{
	wblogf("start dual transpond %d, %d", clientSd, serverSd);
	TranspondThreadParam *p = (TranspondThreadParam *)malloc(sizeof(TranspondThreadParam));
	p->clientSd = clientSd;
	p->serverSd = serverSd;
#ifdef WIN32
	CreateThread(NULL, 0, transpondThread, p, 0, NULL);
#else
	pthread_t pid;
	if (pthread_create(&pid, NULL, transpondThread, p) != 0) {
		wbloglf(LogLevelError, "dualTranspond pthread_create failed");
	}
#endif
	transpond(clientSd, serverSd, myopt.serverPort ? true : false);
	closeSocket(clientSd);
	closeSocket(serverSd);
}

void cHandleAccept(int clientSd, struct sockaddr_in addr)
{
	wblogf("create connection to my server: %s %d", myopt.serverHost, myopt.serverPort);
	int serverSd = createConnection(myopt.serverHost, myopt.serverPort);

	if (serverSd < 0) {
		wbloglf(LogLevelError, "cHandleAccept creat connection failed:%d", serverSd);
	} else {
		wblogf("created server connection:%d", serverSd);
		dualTranspond(clientSd, serverSd);
	}
}

void sHandleAccept(int clientSd, struct sockaddr_in addr)
{
    wblogf("begin read header");
    char header[MAX_HEADER_SIZE];
    memset(header, 0, MAX_HEADER_SIZE);
    if (readHeader(clientSd, header, MAX_HEADER_SIZE) < 0) {
		wbloglf(LogLevelError, "read header error");
		return;
	} 

    capture(header);

    char host[MAX_HOST_SIZE];
    int port;
    int isTunnel;
    if (extractHostPort(header, host, &port, &isTunnel) >= 0) {
        if (isTunnel) {
            wblogf("send tunnel established");
            char *respond = "HTTP/1.1 200 Connection Established\r\n\r\n";
            int cnt;
            if ((cnt = wbsend(clientSd, respond, strlen(respond), 0, true)) < 0) {
                wbloglf(LogLevelError, "send tunnel establish error");
            }
        }
        
        wblogf("create connection to web server: %s %d", host, port);
        int serverSd = createConnection(host, port);

        if (serverSd < 0) {
			wbloglf(LogLevelError, "sHandleAccept creat connection failed:%d", errno);
        } else {
			wblogf("created server connection:%d", serverSd);

			if (!isTunnel) {
            	wblogf("send header:%s", header);
	            wbsend(serverSd, header, strlen(header), 0, false);
			}

			dualTranspond(clientSd, serverSd);
		}
	} else {
		wbloglf(LogLevelError, "extractHostPort error");
	}
}

void handleAccept(int clientSd, struct sockaddr_in addr) {
	if (myopt.serverPort) {
		cHandleAccept(clientSd, addr);
	} else {
		sHandleAccept(clientSd, addr);
	}
}

#ifdef WIN32
DWORD WINAPI acceptThread(LPVOID lpParam)
#else
void *acceptThread(void *lpParam)
#endif
{
	AcceptThreadParam *p = (AcceptThreadParam *)lpParam;
	handleAccept(p->sd, p->addr);
	free(p);
	p = NULL;
    return 0;
}

void start() {
#ifdef WIN32
	WSADATA wsaData;
	WORD sv = MAKEWORD(2, 0);
	if (WSAStartup(sv, &wsaData) != 0) 
	{
		wbloglf(LogLevelError, "init socket dll error!");
		exit(1);
	}
#endif

    int sd;
    if ((sd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        wbloglf(LogLevelError, "socket() error: %d", sd);
        exit(1);
    }

    int opt = 1;
#ifdef WIN32
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
#else
    int sndopt;
    unsigned int lenSndopt = sizeof(sndopt);
    if (getsockopt(sd, SOL_SOCKET, SO_SNDBUF, &sndopt, &lenSndopt) < 0) {
        wbloglf(LogLevelError, "getsockopt SO_SNDBUF error:%d", errno);
    }
    int rcvopt;
    unsigned int lenRcvopt = sizeof(rcvopt);
    if (getsockopt(sd, SOL_SOCKET, SO_RCVBUF, &rcvopt, &lenRcvopt) < 0) {
        wbloglf(LogLevelError, "getsockopt SO_RCVBUF error:%d", errno);
    }
    wblogf("SO_SNDBUF:%d, SO_RCVBUF:%d", sndopt, rcvopt);

    int sndbufSize = 524288;
    if (setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &sndbufSize, sizeof(sndbufSize)) < 0) {
        wbloglf(LogLevelError, "setsockopt SO_SNDBUF error:%d", errno);
    }

    int rcvbufSize = 524288;
    if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &rcvbufSize, sizeof(rcvbufSize)) < 0) {
        wbloglf(LogLevelError, "setsockopt SO_RCVBUF error:%d", errno);
    }

    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
        wbloglf(LogLevelError, "setsockopt SO_REUSEADDR error:%d", errno);
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(myopt.localPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        wbloglf(LogLevelError, "bind error %d", errno);
        exit(1);
    }

    if (listen(sd, SOMAXCONN) != 0) {
        wbloglf(LogLevelError, "listen error");
        exit(1);
    }
    
    wblogf("server started, listen on port: %d", myopt.localPort);
    int len;
    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        int clientSd = accept(sd, (struct sockaddr*)&clientAddr, &clientAddrLen);
		if (clientSd < 0) {
            if (errno == EINTR) {
                wblogf("accept EINTR");
                continue;
            } else {
                wbloglf(LogLevelError, "accept failed: %d", errno);
                break;
            }
		}

    	wblogf("client:%d, %s, %d", clientSd, inet_ntoa(clientAddr.sin_addr), clientAddr.sin_port);

		AcceptThreadParam *p = (AcceptThreadParam *)malloc(sizeof(AcceptThreadParam));
		p->addr = clientAddr;
		p->sd = clientSd;
#ifdef WIN32
		HANDLE handle = CreateThread(NULL, 0, acceptThread, p, 0, NULL);
#else
		pthread_t pid;
		if (pthread_create(&pid, NULL, acceptThread, p) != 0) {
			wbloglf(LogLevelError, "pthread_create failed");
		}
#endif
	}
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

void handleOpt(int argc, char *argv[])
{
	int opt;
    char *ch = NULL;
	while ((opt = getopt(argc, argv, ":dcew:p:h:l:")) != -1) {
		switch (opt) {
			case 'd':
                myopt.daemon = true;
				break;
			case 'p':
                myopt.localPort = atoi(optarg);
                break;
            case 'h':
                ch = strchr(optarg, ':');
                if (ch) {
                    strncpy(myopt.serverHost, optarg, ch - optarg);
                    myopt.serverPort = atoi(ch + 1);
                } else {
                    strcpy(myopt.serverHost, optarg);
                    myopt.serverPort = 80;
                }
                break;
            case 'c':
                myopt.isCapture = 1;
                break;
            case 'w':
                memset(myopt.capturePath, 0, MAX_PATH_SIZE);
                strcpy(myopt.capturePath, optarg);
                break;
            case 'e':
                myopt.isEncrypt = 1;
                break;
                
            default:
                usage();
                exit(1);
		}
	}

    if (!myopt.localPort) {
        printf("please appoint local port\n\n");
        usage();
        exit(1);
    }
}
#ifndef WIN32
void sigchld_handler() {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
#endif

#ifndef WIN32
void catch_crash_signal(int sig) {
    void *array[10];
    size_t size;
    size = backtrace(array, 10);
    wbloglf(LogLevelError, "error signal: %d\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    //wblog(array);
}

void handle_pipe(int sig)
{
    wbloglf(LogLevelError, "SIGPIPE");
}

void setup_signal() {
    signal(SIGILL, catch_crash_signal);
    signal(SIGABRT, catch_crash_signal);
    signal(SIGBUS, catch_crash_signal);
    signal(SIGSEGV, catch_crash_signal);
    signal(SIGSYS, catch_crash_signal);

    struct sigaction action;
    action.sa_handler = handle_pipe;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGPIPE, &action, NULL);
}
#endif

int main(int argc, char *argv[]) {

//    wblogInitContext();
#ifndef WIN32
    setup_signal();
#endif

    handleOpt(argc, argv);
    if (myopt.daemon) {
#ifdef WIN32
#else
		wblogf("daemon");

    	//signal for child process
    	signal(SIGCHLD, sigchld_handler);
        
		pid_t pid = fork();
        if (pid < 0) {
            printf("Could not daemon!\n");
            exit(1);
        } else if (0 == pid) {
            start();
        }
#endif
    } else {
        start();
    }
//    wblogDestroyContext();
}
