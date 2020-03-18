import * as path from 'path';
import * as fse from 'fs-extra';
import { runTests } from 'vscode-test';

const out = path.join(__dirname, '..');

async function main() {
    try {
        const extensionDevelopmentPath = path.join(__dirname, '../../');
        const extensionTestsPath = path.join(__dirname, '.');
        const testWorkspace = path.join(out, 'tmp', 'workspaceFolder');

        await fse.mkdirp(testWorkspace);
        console.log('extensionDevelopmentPath: ', extensionDevelopmentPath);
        console.log('extensionTestsPath: ', extensionTestsPath);
        console.log('testWorkspace: ', testWorkspace);

        await runTests({
            vscodeExecutablePath: undefined,
            version: 'insiders',
            platform: undefined,
            extensionDevelopmentPath: extensionDevelopmentPath,
            extensionTestsPath: extensionTestsPath,
            extensionTestsEnv: { C2_DEBUG: 'true' },
            launchArgs: [testWorkspace, '--disable-extensions']
        });

        process.exit(0);
    } catch (err) {
        console.error('Failed to run tests');
        process.exit(1);
    }
}

main();