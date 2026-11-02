const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');

const executable = path.resolve(process.argv[2]);
const resources = path.join(__dirname, '../../src/server');


async function fixture(t) {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'async-log-cloud-storage-startup-tests-'));
    const processes = [];
    t.after(async () => {
        for (const job of processes) {
            if (job.child.exitCode === null && job.child.signalCode === null) job.child.kill('SIGKILL');
            await job.closed;
        }
        fs.rmSync(directory, { recursive: true, force: true });
    });

    const reservation = net.createServer().listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const port = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));

    const binary = path.join(directory, 'server');
    fs.copyFileSync(executable, binary);
    fs.mkdirSync(path.join(directory, 'config'));
    fs.mkdirSync(path.join(directory, 'web'));
    fs.copyFileSync(path.join(resources, 'web/template.html'), path.join(directory, 'web/template.html'));
    const config = JSON.parse(fs.readFileSync(path.join(resources, 'config/server.conf'), 'utf8'));
    config.server_ip = '127.0.0.1';
    config.server_port = port;
    config.metadata_file = './metadata/state/files.json';
    fs.writeFileSync(path.join(directory, 'config/server.conf'), JSON.stringify(config));

    const url = `http://127.0.0.1:${port}`;
    return {
        directory,
        port,
        url,
        metadata: path.join(directory, config.metadata_file),
        start() {
            // 工作目录与程序目录不同，确认相对配置路径仍基于可执行文件解析。
            const child = spawn(binary, [], { cwd: os.tmpdir(), stdio: ['ignore', 'ignore', 'pipe'] });
            const job = { child, closed: once(child, 'close'), stderr: '' };
            child.stderr.on('data', data => { job.stderr += data; });
            processes.push(job);
            return job;
        },
        async ready(job) {
            const deadline = Date.now() + 5000;
            while (Date.now() < deadline) {
                assert.equal(job.child.exitCode, null, job.stderr);
                assert.equal(job.child.signalCode, null, job.stderr);
                try {
                    const response = await fetch(url, { signal: AbortSignal.timeout(300) });
                    assert.equal(response.status, 200);
                    return await response.text();
                } catch (error) {
                    if (error.code === 'ERR_ASSERTION') throw error;
                    await delay(20);
                }
            }
            assert.fail(`server did not become ready: ${job.stderr}`);
        }
    };
}

test('first startup creates storage, serves HTTP, and restores uploads after a clean restart', { timeout: 15000 }, async t => {
    const app = await fixture(t);
    let job = app.start();
    const page = await app.ready(job);
    assert.ok(page.includes('已上传文件'));
    assert.equal(fs.readFileSync(app.metadata, 'utf8'), '[]');
    for (const type of ['compressed', 'uncompressed'])
        assert.ok(fs.statSync(path.join(app.directory, 'data', type)).isDirectory());

    const files = [];
    const content = Buffer.concat([Buffer.alloc(4096, 'A'), Buffer.from([0, 0xff, 1, 0x80])]);
    for (const type of ['uncompressed', 'compressed']) {
        const name = `中文 ${type}.bin`;
        const response = await fetch(`${app.url}/upload`, {
            method: 'POST',
            headers: { FileName: Buffer.from(name).toString('base64'), StorageType: type },
            body: content
        });
        assert.equal(response.status, 201);
        files.push({ name, url: (await response.json()).url });
    }

    job.child.kill('SIGINT');
    assert.deepEqual(await job.closed, [0, null], job.stderr);
    const metadata = fs.readFileSync(app.metadata, 'utf8');

    job = app.start();
    const restoredPage = await app.ready(job);
    assert.equal(fs.readFileSync(app.metadata, 'utf8'), metadata);
    for (const file of files) {
        assert.ok(restoredPage.includes(file.name));
        const response = await fetch(app.url + file.url);
        assert.equal(response.status, 200);
        assert.deepEqual(Buffer.from(await response.arrayBuffer()), content);
    }
    job.child.kill('SIGTERM');
    assert.deepEqual(await job.closed, [0, null], job.stderr);
    assert.equal(fs.readFileSync(app.metadata, 'utf8'), metadata);
});

for (const [description, content] of [['empty', ''], ['malformed JSON', '{broken'], ['wrong root type', '{}']]) {
    test(`startup fails without overwriting ${description} metadata`, { timeout: 10000 }, async t => {
        const app = await fixture(t);
        fs.mkdirSync(path.dirname(app.metadata), { recursive: true });
        fs.writeFileSync(app.metadata, content);
        const job = app.start();
        assert.deepEqual(await job.closed, [1, null]);
        assert.match(job.stderr, /Server failed:/);
        assert.equal(fs.readFileSync(app.metadata, 'utf8'), content);
    });
}

test('startup reports an occupied port and keeps existing metadata', { timeout: 10000 }, async t => {
    const app = await fixture(t);
    const listener = net.createServer().listen(app.port, '127.0.0.1');
    await once(listener, 'listening');
    t.after(() => new Promise(resolve => listener.close(resolve)));
    fs.mkdirSync(path.dirname(app.metadata), { recursive: true });
    fs.writeFileSync(app.metadata, '\n[]\n');
    const job = app.start();
    assert.deepEqual(await job.closed, [1, null]);
    assert.match(job.stderr, /Server failed:.*evhttp_bind_socket\(\) failed/);
    assert.equal(fs.readFileSync(app.metadata, 'utf8'), '\n[]\n');
});

test('startup reports a storage directory creation failure without replacing the existing file', { timeout: 10000 }, async t => {
    const app = await fixture(t);
    const occupied = path.join(app.directory, 'data');
    fs.writeFileSync(occupied, 'keep');
    const job = app.start();
    assert.deepEqual(await job.closed, [1, null]);
    assert.match(job.stderr, /Server failed:/);
    assert.equal(fs.readFileSync(occupied, 'utf8'), 'keep');
    assert.equal(fs.existsSync(app.metadata), false);
});
