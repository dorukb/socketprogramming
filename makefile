make:
	gcc server.c -o server
	gcc client.c -o client
	g++ servertroll.cpp -o trollserver -lpthread
	gcc clienttroll.c -o trollclient -lpthread
