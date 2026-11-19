const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const pagePath = path.join(__dirname, '../../src/client/web/index.html');
const html = fs.readFileSync(pagePath, 'utf8');
const script = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)].map(match => match[1]).join('\n');


function element(tag) {
    return {
        tag,
        children: [],
        textContent: '',
        append(...children) {
            this.children.push(...children);
        },
        replaceChildren(fragment) {
            this.children = fragment ? fragment.children : [];
        }
    };
}

// 直接执行真实页面脚本，模拟 DOM 和 fetch；不监听端口，也不依赖第三方包或真实浏览器。
function loadPage(pageUrl, fetch, fileInput, storageType) {
    const list = element('div');
    const status = element('p');
    const alerts = [];
    const elements = { fileList: list, fileListStatus: status, fileInput };
    const context = vm.createContext({
        document: {
            getElementById: id => elements[id],
            createElement: element,
            createDocumentFragment: () => element('#fragment'),
            querySelector(selector) {
                assert.equal(selector, 'input[name="storageType"]:checked');
                return { value: storageType };
            }
        },
        btoa: value => Buffer.from(value, 'binary').toString('base64'),
        URL,
        fetch,
        location: new URL(pageUrl),
        alert: message => alerts.push(message),
        console: { log() { }, error() { } }
    });
    const loaded = vm.runInContext(script, context, { filename: pagePath });
    return { loaded, list, status, alerts, uploadFile: context.uploadFile };
}

function listResponse(files) {
    return { ok: true, json: async () => ({ files }) };
}

const listedFile = {
    name: '中文 <img src=x> +%2F.bin',
    storage_type: 'compressed',
    stored_size_bytes: 1536,
    modified_time: '2000-01-01 08:00:00',
    download_url: '/download/%E4%B8%AD%E6%96%87%20%3Cimg%20src%3Dx%3E%20%2B%252F.bin'
};

for (const pageUrl of [
    'http://127.0.0.1:7081/',
    'http://localhost:7081/',
    'http://cloud.example:7081/',
    'https://cloud.example/'
]) {
    for (const storageType of ['uncompressed', 'compressed']) {
        test(`${storageType} upload stays on ${pageUrl}`, async () => {
            const file = {
                name: '中文 空格+%2F.bin',
                bytes: Buffer.from([0x41, 0x00, 0xff, 0x7a])
            };
            const requests = [];
            const fileInput = { files: [file], value: file.name };
            const page = loadPage(pageUrl, async (url, options) => {
                requests.push({ url: new URL(url, pageUrl).href, options });
                if (url === '/upload') return { ok: true };
                return listResponse(requests.length === 1 ? [] : [{ ...listedFile, storage_type: storageType }]);
            }, fileInput, storageType);

            assert.equal(page.status.textContent, '正在加载文件列表…');
            await page.loaded;
            assert.equal(page.status.textContent, '暂无上传文件');
            assert.equal(page.list.children.length, 0);
            await page.uploadFile();

            assert.deepEqual(requests.map(request => request.url), ['/api/files', '/upload', '/api/files']
                .map(url => new URL(url, pageUrl).href));
            const upload = requests[1].options;
            assert.equal(upload.method, 'POST');
            assert.equal(upload.headers.StorageType, storageType);
            assert.equal(upload.headers.FileName, Buffer.from(file.name, 'utf8').toString('base64'));
            assert.equal(upload.body, file);
            assert.equal(fileInput.value, '');
            assert.equal(page.status.textContent, '');
            assert.deepEqual(page.alerts, []);

            assert.equal(page.list.children.length, 1);
            const [info, download] = page.list.children[0].children;
            assert.deepEqual(info.children.map(field => field.textContent), [
                `📄${listedFile.name}`,
                storageType === 'compressed' ? '压缩存储' : '未压缩存储',
                '存储大小：1.50 KB',
                listedFile.modified_time
            ]);
            assert.equal(download.tag, 'a');
            assert.equal(download.href, listedFile.download_url);
        });
    }
}

test('the initial list formats stored sizes in binary units', async () => {
    const sizes = [0, 1023, 1024, 1024 ** 2, 1024 ** 3];
    const files = sizes.map((stored_size_bytes, index) => ({
        ...listedFile,
        name: `${index}.bin`,
        download_url: `/download/${index}.bin`,
        stored_size_bytes
    }));
    const page = loadPage('https://cloud.example/', async () => listResponse(files));
    await page.loaded;

    assert.deepEqual(page.list.children.map(row => row.children[0].children[2].textContent), [
        '存储大小：0.00 B',
        '存储大小：1023.00 B',
        '存储大小：1.00 KB',
        '存储大小：1.00 MB',
        '存储大小：1.00 GB'
    ]);
});

for (const [description, fetch] of [
    ['HTTP 500', async () => ({ ok: false, status: 500 })],
    ['network error', async () => { throw new Error('offline'); }],
    ['unsafe download URL', async () => listResponse([{ ...listedFile, download_url: '/\\outside.example/file' }])]
]) {
    test(`file list shows a failure for ${description}`, async () => {
        const page = loadPage('https://cloud.example/', fetch);
        await page.loaded;
        assert.equal(page.status.textContent, '文件列表加载失败，请刷新页面重试。');
        assert.equal(page.list.children.length, 0);
    });
}

test('a slow initial list response cannot overwrite the list refreshed after upload', async () => {
    let resolveInitialList;
    const initialList = new Promise(resolve => { resolveInitialList = resolve; });
    let listRequests = 0;
    const page = loadPage('https://cloud.example/', async url => {
        if (url === '/upload') return { ok: true };
        return ++listRequests === 1 ? initialList : listResponse([listedFile]);
    }, { files: [{ name: listedFile.name }] }, 'compressed');

    await page.uploadFile();
    resolveInitialList(listResponse([]));
    await page.loaded;

    assert.equal(page.list.children.length, 1);
    assert.equal(page.status.textContent, '');
});
