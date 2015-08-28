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

//basic macro

//def macro

//global variable
char g_remote_host[128];
int g_remote_port;
int g_local_port;

//func declare
void start_server();
int create_server_sock();
void server_loop();
void handle_client();
int read_header();
int extract_host();

//func implement

//main

