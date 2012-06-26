Simple Http Server implementation comparisons
==========

## HttpListener ##
Project used to start a Http Server that responds back a 500 byte payload response over http://+:80/Server/
 - The Server is based of HTTP Server Version 2.0

## ManagedHttpListener ##
Http Server implementation using System.Net which returns a 500 byte payload on a GetRequest without body to http://+:80/server

## Test ##

wcat is used to run these tests using the settings.ubr and the client.ubr files.

- Start the Controller
```
start wcctl.exe -t client.ubr -f settings.ubr -s %COMPUTERNAME% -v 2 -c 20 -o output.xml -x 
```

- Start Clients 
```
for /l %%i in (1,1,20) do start wcclient.exe %COMPUTERNAME%
```


## References ##

- [WCAT Download (64-bit)](http://www.iis.net/community/default.aspx?tabid=34&g=6&i=1467)
- HTTP Server Version 2.0 - http://msdn.microsoft.com/en-us/library/windows/desktop/aa364705(v=vs.85).aspx