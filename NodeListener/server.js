// we are running self-hosted
var cluster = require('cluster');
var numCPUs = require('os').cpus().length;

if (cluster.isMaster) {        
    for (var i = 0; i < numCPUs; i++) {
        cluster.fork();
    }
    console.log('Server Started!!!');
}
else{
    // cluster's worker process
    setupServer();
}

function setupServer() {
    var http = require('http');
    var content = new Array();
    for (var i = 0; i < 500; i++) {
        content[i] = 'a';
    }
    content[500] = '\n';
    content = content.join("");    
    var head =  { 'Content-Type': 'text/html' };
    var server = http.createServer(function (req, res) {
        res.writeHead(200, head);        
        res.end(content);
    });
    server.listen(80);    
}