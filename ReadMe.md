Simple Http Server implementation comparisons
==========

## HttpListener ##
Project used to start a listener that sends back a 500 byte payload response over http://+:80/Server/

## ManagedHttpListener ##
Http Server implementation using System.Net which returns a 500 byte payload on a GetRequest without body to http://+:80/server

## Test ##

wcat is used to run these tests using the settings.ubr and the client.ubr files.

