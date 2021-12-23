import socket

HOST = ''
PORT = 5000
clientsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) # UDP uses Datagram, but not stream
clientsock.bind((HOST, PORT))
print "Waiting for packets..."
while True:
  data, addr = clientsock.recvfrom(1024)
  print "Received ->", data
  clientsock.sendto(data, addr)
  break
