/*
create server sock
accept client sock
read header
extract host if remote host not set
create remote sock
forward header
forward data
revc data from client sock
send data to remote sock
revc data from remote sock
send data to client sock
*/
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

//basic macro

//def macro
#define BUF_SIZE 8192

//global variable
char g_remote_host[128];
int g_remote_port;

int g_server_port;

int g_server_sock;
int g_client_sock;
int g_remote_sock;

//func declare
void start_server();
int create_server_sock();
void server_loop();
void handle_client(int client_sock, struct sockaddr_in client_addr);
int read_header(int client_sock, char *buffer);
int extract_host(char *header, char *host, int *port);
int create_remote_sock(char *host, int port);
void rewrite_header(char *header);
int forward_header(char *header, int dest_sock);
int forward_data(int src_sock, int dest_sock);

ssize_t read_sock_line(int sock, void *buffer, size_t n);
int send_tunnel_ok(int client_sock);

//func implement
void start_server()
{
	printf("start_server\n");

	if ((g_server_sock = create_server_sock()) < 0)
	{
		perror("create_server_sock error\n");
	}

	server_loop();
}

int create_server_sock()
{
	printf("create_server_sock\n");
	
	int server_sock, optval;
	struct sockaddr_in server_addr;	

	if ((server_sock = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("create server sock error\n");
		return -1;
	}

	if ((setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) < 0)
	{
		perror("set server sock opt error\n");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(g_server_port);

	if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0)
	{
		perror("bind server sock error");
		return -1;
	}

	if (listen(server_sock, 100) < 0)
	{
		perror("listen server sock error");
		return -1;
	}

	return server_sock;
}

void server_loop()
{
	printf("server_loop\n");

	struct sockaddr_in client_addr;
	socklen_t addrlen = sizeof(client_addr);

	while(1)
	{
		g_client_sock =  accept(g_server_sock, (struct sockaddr*)&client_addr, &addrlen);
        pid_t fpid = fork();
        if (fpid < 0)
            perror("fork error\n");
        else if (fpid == 0)
        {
            printf("child process\n");
            close(g_server_sock);
            handle_client(g_client_sock, client_addr);
            exit(0);
        } else
            printf("parent process\n");
            close(g_client_sock);
	}
}

void handle_client(int client_sock, struct sockaddr_in client_addr)
{
	printf("handle_client\n");

    int is_http_tunnel = 0;
    char host[128];
    memset(&host, 0, sizeof(host));
    
    int port = 0;
    
    char header_buffer[BUF_SIZE];
    memset(&header_buffer, 0, sizeof(header_buffer));
    
    if (read_header(client_sock, header_buffer) < 0)
        return;
    
    char *p = strstr(header_buffer, "CONNECT");
    if (p)
        is_http_tunnel = 1;
    
    if (strlen(g_remote_host) == 0)
    {
        if (extract_host(header_buffer, host, &port) < 0)
            return;
    } else
    {
        strncpy(host, g_remote_host, sizeof(g_remote_host));
        port = g_remote_port;
    }
    
	if ((g_remote_sock = create_remote_sock(host, port)) < 0)
        return;

    if (fork() == 0)
    {
//        if (!is_http_tunnel) {
            forward_header(header_buffer, g_remote_sock);
//        }
        forward_data(g_client_sock, g_remote_sock);
        exit(0);
    }

    if (fork() == 0) {
        if (is_http_tunnel) {
//            send_tunnel_ok(g_client_sock);
        }
        forward_data(g_remote_sock, g_client_sock);
        exit(0);
    }
    
    close(g_remote_sock);
    close(g_client_sock);
}

int read_header(int client_sock, char *buffer)
{
	printf("read_header\n");
    
	char line_buffer[2048];
	char *base_ptr = buffer;
	while(1)
	{
		memset(&line_buffer, 0, sizeof(line_buffer));
		ssize_t total_read = read_sock_line(client_sock, &line_buffer, sizeof(line_buffer));
		printf("%s", line_buffer);
		if (total_read <= 0)
			return -1;

		if (base_ptr + total_read - buffer < BUF_SIZE)
		{
			strncpy(base_ptr, line_buffer, total_read);
			base_ptr += total_read;
		} else
		{
			perror("client sock read header over flow");
			return -1;
		}

		if (strcmp(line_buffer, "\r\n") == 0 || strcmp(line_buffer, "\n") == 0)
		{
			break;
		}
	}
	return 0;
}

int extract_host(char *header, char *host, int *port)
{
	printf("extract_host\n");
    
    char *start = strstr(header, "Host:");
    if (!start)
        return -1;
    
    char *end = strchr(start, '\n');
    if (!end)
        return -1;
    
    char *p = strchr(start+5, ':');//port
    
    if (p && p < end)
    {
        int port_len = (int)(end - p - 1);
        char port_s[port_len];
        strncpy(port_s, p+1, port_len);
        port_s[port_len] = '\0';
        *port = atoi(port_s);
        
        int host_len = (int)(p - start - 5 - 1);
        strncpy(host, start+5+1, host_len);
        host[host_len] = '\0';
    } else
    {
        *port = 80;
        
        int host_len = (int)(end - start - 5 - 1 - 1);
        strncpy(host, start+5+1, host_len);
        host[host_len] = '\0';
    }
    
    printf("host:%s\n", host);
    printf("port:%d\n", *port);
	return 0;
}

int create_remote_sock(char *host, int port)
{
	printf("create_remote_sock\n");
    
    int remote_sock, optval;
    struct sockaddr_in remote_addr;
    struct hostent *server;
    
    if ((remote_sock = socket(PF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    
//    if (setsockopt(remote_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
//        return -1;
    
    if ((server = gethostbyname(host)) == NULL)
        return -1;
    
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(port);
    memcpy(&remote_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    
    if (connect(remote_sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0)
        return -1;
    
	return remote_sock;
}

void rewrite_header(char *header)
{
    printf("rewrite_header\n");
    
    char * p = strstr(header,"http://");
    char * p0 = strchr(p,'\0');
    char * p5 = strstr(header,"HTTP/"); /* "HTTP/" 是协议标识 如 "HTTP/1.1" */
    size_t len = strlen(header);
    if(p)
    {
        char * p1 = strchr(p + 7,'/');
        if(p1 && (p5 > p1))
        {
            //转换url到 path
            memcpy(p,p1,(int)(p0 -p1));
            size_t l = len - (p1 - p) ;
            header[l] = '\0';
            
            
        } else
        {
            char * p2 = strchr(p,' ');  //GET http://3g.sina.com.cn HTTP/1.1
            
            // printf("%s\n",p2);
            memcpy(p + 1,p2,(int)(p0-p2));
            *p = '/';  //url 没有路径使用根
            size_t l  = len - (p2  - p ) + 1;
            header[l] = '\0';
        }
    }
}

int forward_header(char *header, int dest_sock)
{
	printf("forward_header\n");
    
    rewrite_header(header);
    if (send(dest_sock, header, strlen(header), 0) < 0)
        return -1;
	return 0;
}

int forward_data(int src_sock, int dest_sock)
{
	printf("forward_data\n");
    
    char buffer[BUF_SIZE];
    size_t read_size;

    while ((read_size = recv(src_sock, buffer, BUF_SIZE, 0)) > 0)
    {
        send(dest_sock, buffer, read_size, 0);
    }
    
    shutdown(dest_sock, SHUT_RDWR);
    shutdown(src_sock, SHUT_RDWR);
	return 0;
}


ssize_t read_sock_line(int sock, void *buffer, size_t n)
{
	ssize_t num_read;
	size_t total_read;
	char *buf;
	char ch;

	if (n <=0 || buffer == NULL)
	{
        errno = EINVAL;
		return -1;
	}

	buf = buffer;
	total_read = 0;
	while(1)
	{
		num_read = recv(sock, &ch, 1, 0);
	
		if (num_read == -1)
		{
            if (errno == EINTR)
                continue;
            else
                return -1;
		} else if (num_read == 0)
        {
            if (total_read == 0)
                return 0;
            else
                return -1;
        } else
		{
			if (total_read < n - 1)
			{
				total_read++;
				*buf++ = ch;
			}

			if (ch == '\n')
				break;
		}
	}

	*buf = '\0';
	return total_read;
}

int send_tunnel_ok(int client_sock)
{
    char * resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
    int len = strlen(resp);
    char buffer[len+1];
    strcpy(buffer,resp);
    if(send(client_sock,buffer,len, 0) < 0)
    {
        perror("Send http tunnel response  failed\n");
        return -1;
    }
    return 0;
}

//main
int main(int argc, char *argv[])
{
	g_server_port = 5000;
	start_server();
}
