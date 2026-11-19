const fs = require('node:fs');
const http = require('node:http');
const { isIP } = require('node:net');
const path = require('node:path');
const { pipeline } = require('node:stream');

const [backendIp = '127.0.0.1', backendPort = '7081', webPort = '7080', ...extraArgs] = process.argv.slice(2);
const validPort = port => /^\d+$/.test(port) && Number(port) >= 1 && Number(port) <= 65535;
if (extraArgs.length || !isIP(backendIp) || !validPort(backendPort) || !validPort(webPort)) {
    console.error('用法：node src/client/web/main.js [后端 IP] [后端端口] [前端端口]');
    process.exit(1);
}

const page = fs.readFileSync(path.join(__dirname, 'index.html'));


// Connection 声明的头字段只适用于当前连接，不随请求或响应转发。
function forwardedHeaders(headers) {
    const forwarded = { ...headers };
    const connectionHeaders = (headers.connection || '').toLowerCase().split(',');
    for (const name of [
        'connection', 'keep-alive', 'proxy-connection', 'proxy-authenticate',
        'proxy-authorization', 'te', 'trailer', 'transfer-encoding', 'upgrade',
        ...connectionHeaders
    ]) {
        delete forwarded[name.trim()];
    }
    return forwarded;
}

const server = http.createServer((req, res) => {
    const route = req.url.split('?', 1)[0];
    if ((req.method === 'GET' || req.method === 'HEAD') && (route === '/' || route === '/index.html')) {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': page.length });
        res.end(page);
        return;
    }

    const headers = forwardedHeaders(req.headers);
    delete headers.host;
    const upstream = http.request({
        host: backendIp, port: Number(backendPort), method: req.method, path: req.url, headers, timeout: 30_000
    }, reply => {
        res.writeHead(reply.statusCode, forwardedHeaders(reply.headers));
        pipeline(reply, res, () => { });
    });

    upstream.on('timeout', () => upstream.destroy(new Error('后端请求超时')));
    upstream.on('error', error => {
        if (res.destroyed) return;
        console.error(`转发 ${req.method} 请求失败 (${error.code || error.message})`);
        if (res.headersSent) {
            res.destroy();
            return;
        }
        res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8', Connection: 'close' });
        res.end('无法连接后端，请检查 IP、端口及后端运行状态。');
    });
    res.on('close', () => {
        if (!res.writableFinished) upstream.destroy();
    });
    req.pipe(upstream);
});


server.on('error', error => {
    console.error(`无法在 127.0.0.1:${webPort} 启动前端：${error.message}`);
    process.exitCode = 1;
});

server.listen(Number(webPort), '127.0.0.1', () => {
    console.log(`Web 客户端：http://127.0.0.1:${webPort}/`);
    console.log(`后端：${isIP(backendIp) === 6 ? `[${backendIp}]` : backendIp}:${backendPort}`);
});
