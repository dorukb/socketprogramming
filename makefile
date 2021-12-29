make:
	g++ servertroll.cpp -o server -lpthread
	g++ clienttroll.cpp -o client -lpthread
	
clear:
	rm server0 client0 sv.debug cl.debug
