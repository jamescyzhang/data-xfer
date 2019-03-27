all: receiver sender

receiver: receiver.o
	gcc packetTransfer.c -o receiver receiver.c

sender: sender.o
	gcc packetTransfer.c -o sender sender.c

clean:
	$(RM) sender
	$(RM) receiver
