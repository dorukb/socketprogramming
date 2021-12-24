make:
	g++ servertroll.cpp -o trollserver -lpthread
	gcc clienttroll.c -o trollclient -lpthread
