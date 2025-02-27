build:
	mpic++ -g bit_torrent.cpp -o bit_torrent -pthread -Wall -std=c++17

clean:
	rm -rf bit_torrent
