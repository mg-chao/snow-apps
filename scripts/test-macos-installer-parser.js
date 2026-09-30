#!/usr/bin/env node
// Exercise the installer's shipped JXA release parser without macOS or a network.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');

const installer = fs.readFileSync(path.join(__dirname, 'install-snow-shot-macos.sh'), 'utf8');
const sandbox = {
    ObjC: {import() {}, unwrap(value) { return value; }},
    $: {
        NSData: {dataWithContentsOfFile(file) { return fs.readFileSync(file, 'utf8'); }},
        NSString: {alloc: {initWithDataEncoding(value) { return value; }}},
        NSUTF8StringEncoding: 'utf8',
    },
};
function parser(name) {
    const source = installer.match(new RegExp(`${name}\\(\\) \\{[\\s\\S]*?<<'JXA'\\r?\\n([\\s\\S]*?)\\r?\\nJXA`));
    assert(source, `${name} must be present`);
    vm.runInNewContext(source[1], sandbox);
    return sandbox.run;
}
const releaseUrls = parser('release_urls');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'snow-installer-parser-'));
try {
    const releaseFile = path.join(directory, 'releases.json');
    const attachmentFile = path.join(directory, 'attachments.json');
    const asset = (version, host, suffix = '', product = 'snow-shot') => {
        const tag = `v${version}_snow-shot`;
        const name = `${product}-${version}-macos-arm64.dmg${suffix}`;
        return {name, browser_download_url:
            `https://${host}/mg-chao/snow-apps/releases/download/${tag}/${name}`};
    };
    const release = (version, host, id) => ({tag_name: `v${version}_snow-shot`, draft: false,
        prerelease: version.includes('-'), id,
        assets: [asset(version, host), asset(version, host, '.sha256')]});
    const older = release('2.0.0', 'github.com', 123);
    const preview = release('3.0.0-beta.2', 'github.com', 456);
    const invalid = release('4.0.0', 'github.com', 789);
    invalid.assets.pop();
    fs.writeFileSync(releaseFile, JSON.stringify([older, invalid, preview]));
    const selected = releaseUrls([releaseFile, 'arm64', 'github', '', '']);
    assert(selected.startsWith('v3.0.0-beta.2_snow-shot\n'));
    assert(releaseUrls([releaseFile, 'arm64', 'github', older.tag_name, ''])
        .startsWith(`${older.tag_name}\n`));
    preview.draft = true;
    fs.writeFileSync(releaseFile, JSON.stringify([older, invalid, preview]));
    assert(releaseUrls([releaseFile, 'arm64', 'github', '', '']).startsWith(`${older.tag_name}\n`));

    const giteeOlder = release('2.0.0', 'gitee.com', 123);
    const giteeNewer = release('3.0.0-beta.2', 'gitee.com', 456);
    fs.writeFileSync(releaseFile, JSON.stringify([giteeOlder, giteeNewer].map(
        ({assets, ...metadata}) => metadata)));
    assert.equal(releaseUrls([releaseFile, 'arm64', 'gitee-list', '', '']),
        'v3.0.0-beta.2_snow-shot 456\nv2.0.0_snow-shot 123');
    fs.writeFileSync(attachmentFile, JSON.stringify(giteeOlder.assets));
    assert(releaseUrls([releaseFile, 'arm64', 'gitee', giteeOlder.tag_name, attachmentFile])
        .includes('https://gitee.com/'));
    giteeOlder.assets[0].browser_download_url = 'https://example.invalid/package.dmg';
    fs.writeFileSync(attachmentFile, JSON.stringify(giteeOlder.assets));
    assert.throws(() => releaseUrls([releaseFile, 'arm64', 'gitee', giteeOlder.tag_name, attachmentFile]));

    const paired = release('3.0.0-beta.2', 'github.com', 456);
    paired.assets.push(asset('3.0.0-beta.2', 'github.com', '', 'snow-shot-mini'),
        asset('3.0.0-beta.2', 'github.com', '.sha256', 'snow-shot-mini'));
    fs.writeFileSync(releaseFile, JSON.stringify([older, paired]));
    const miniSelected = releaseUrls([releaseFile, 'arm64', 'github', '', '', 'snow-shot-mini']);
    assert(miniSelected.startsWith(`${paired.tag_name}\n`));
    assert(miniSelected.includes('/snow-shot-mini-3.0.0-beta.2-macos-arm64.dmg\n'));
    assert(!miniSelected.includes('/snow-shot-3.0.0-beta.2-macos-arm64.dmg\n'));
    assert.throws(() => releaseUrls([releaseFile, 'arm64', 'github', older.tag_name, '', 'snow-shot-mini']));
    paired.assets.pop();
    fs.writeFileSync(releaseFile, JSON.stringify([older, paired]));
    assert.throws(() => releaseUrls([releaseFile, 'arm64', 'github', '', '', 'snow-shot-mini']));

    const miniGitee = release('3.0.0-beta.2', 'gitee.com', 456);
    miniGitee.assets.push(asset('3.0.0-beta.2', 'gitee.com', '', 'snow-shot-mini'),
        asset('3.0.0-beta.2', 'gitee.com', '.sha256', 'snow-shot-mini'));
    fs.writeFileSync(attachmentFile, JSON.stringify(miniGitee.assets));
    assert(releaseUrls([releaseFile, 'arm64', 'gitee', miniGitee.tag_name, attachmentFile, 'snow-shot-mini'])
        .includes('/snow-shot-mini-3.0.0-beta.2-macos-arm64.dmg\n'));
    // Preserve the original pagination fixture below.
    fs.writeFileSync(releaseFile, JSON.stringify([giteeOlder, giteeNewer].map(
        ({assets, ...metadata}) => metadata)));

    const count = parser('release_page_count');
    assert.equal(count([releaseFile]), '2');
    const secondPage = path.join(directory, 'second-page.json');
    fs.writeFileSync(secondPage, JSON.stringify([older]));
    const merge = parser('merge_release_pages');
    assert.equal(JSON.parse(merge([releaseFile, secondPage])).length, 3);
    console.log('PASS: macOS installer release parser, SemVer, previews, exact assets, and Gitee attachments.');
} finally {
    const resolved = path.resolve(directory);
    const temporary = path.resolve(os.tmpdir()) + path.sep;
    assert(resolved.startsWith(temporary) &&
           path.basename(resolved).startsWith('snow-installer-parser-'));
    fs.rmSync(resolved, {recursive: true, force: true});
}
