windows
gcc wblog.c wbproxy.c -lwsock32 -o xx

unix-like
gcc wblog.c wbproxy.c -lpthread -o xx
