make:
	g++ servertroll.cpp -o trollserver -lpthread
	g++ clienttroll.cpp -o trollclient -lpthread
