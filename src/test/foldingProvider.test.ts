var chai = require("chai");
chai.config.includeStack = true;
var assert = chai.assert;
import * as path from 'path';
import * as glob from 'glob';
import * as fse from 'fs-extra';
import * as vscode from 'vscode';
import { FoldingRange } from 'vscode'
import FoldingProvider from '../foldingProvider'

describe(path.basename(__filename), function () {
    // Initialize provider
    let provider = new FoldingProvider(true);
    (<any>provider).updateConfig();

    // Get paths
    const extensionDevelopmentPath = path.join(__dirname, '../../');
    const test_files = path.join(extensionDevelopmentPath, 'src', 'test', 'test-files');
    const test_files_results = path.join(extensionDevelopmentPath, 'src', 'test', 'results');

    it('Dump test files', async function () {
        let files = glob.sync('**/**', { cwd: test_files });
        let dumped = 0;
        for (let i = 0; i < files.length; i++) {
            let fullFilePath = path.join(test_files, files[i]);
            let doc = await vscode.workspace.openTextDocument(fullFilePath);
            const maxLines = 10000;
            let buffer = new Array<string>(maxLines);
            let nbuffer = 0;

            // Get ranges & check it
            assert.strictEqual(doc.lineCount < maxLines, true);
            let ranges_undef = provider.provideFoldingRanges(doc);
            assert.notStrictEqual(ranges_undef, undefined);
            let ranges = (<FoldingRange[]>ranges_undef);
            ranges.sort((n1, n2) => n1.start - n2.start);

            // Copy file document to buffer
            const lineCount = doc.lineCount;
            for (let j = 0; j < lineCount; j++) {
                buffer[nbuffer] = doc.lineAt(j).text;
                nbuffer++;
            }

            // Check single range bounds & append fold indicator
            let counter = 0;
            for (let j = 0; j < ranges.length; j++) {
                assert.strictEqual(ranges[j].start <= ranges[j].end, true);
                assert.strictEqual(ranges[j].start >= 0 && ranges[j].start < lineCount, true);
                assert.strictEqual(ranges[j].end > 0 && ranges[j].end < lineCount, true);
                const foldIndicator = " @_" + counter + "_";
                buffer[ranges[j].start] = buffer[ranges[j].start] + foldIndicator;
                buffer[ranges[j].end] = buffer[ranges[j].end] + foldIndicator;
                counter++;
            }

            // Copy buffer to file
            const outFile = path.join(test_files_results, files[i]);
            buffer.length = lineCount;
            fse.outputFile(outFile, buffer.join('\n'));
            console.log("Dump file: " + outFile);
            dumped++;
        }

        assert.strictEqual(dumped, files.length);
    })

    // Could also check against old dumped files, but for now it seems fine to just
    // check the git diff files
});