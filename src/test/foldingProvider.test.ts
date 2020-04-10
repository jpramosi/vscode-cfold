var chai = require("chai");
chai.config.includeStack = true;
var assert = chai.assert;
import * as path from 'path';
import * as glob from 'glob';
import * as fse from 'fs-extra';
import * as vscode from 'vscode';
import { FoldingRange } from 'vscode'
import FoldingProvider from '../foldingProvider'
import { globalConfig, updateConfig } from '../globalConfig';

/// Set default option for the folding provider.
async function setDefaultOptions() {
    await globalConfig.update('class.enable', true);
    await globalConfig.update('commentQuote.enable', true);
    //globalConfig.update('commentSlash.enable', true);
    await globalConfig.update('documentationQuote.enable', true);
    //globalConfig.update('documentationSlash.enable', true);
    await globalConfig.update('enum.enable', true);
    await globalConfig.update('function.enable', true);
    await globalConfig.update('namespace.enable', true);
    await globalConfig.update('preprocessor.enable', true);
    await globalConfig.update('preprocessor.ignoreGuard', true);
    await globalConfig.update('preprocessor.minLines', 0);
    await globalConfig.update('preprocessor.recursiveDepth', 1);
    await globalConfig.update('struct.enable', true);
    await globalConfig.update('withinFunction.enable', true);
    await globalConfig.update('withinFunction.minLines', 0);
    await globalConfig.update('caseLabel.enable', true);
    await globalConfig.update('caseLabel.minLines', 0);
    updateConfig();
}

/// Handle specific files and set the options accordingly.
async function handleFileOptions(file: string) {
    switch (file) {
        case "switch.cpp": {
            //await globalConfig.update('withinFunction.enable', false);
            break;
        }
        default: {
            break;
        }
    }
    updateConfig();
}

describe(path.basename(__filename), function () {
    // Initialize provider
    let provider = new FoldingProvider(true);
    updateConfig();

    // Get paths
    const extensionDevelopmentPath = path.join(__dirname, '../../');
    const test_files = path.join(extensionDevelopmentPath, 'src', 'test', 'test-files');
    const test_files_results = path.join(extensionDevelopmentPath, 'src', 'test', 'results');

    it('Dump test files', async function () {
        let files = glob.sync('**/**', { cwd: test_files });
        let dumped = 0;

        await setDefaultOptions();
        (<any>provider).updateConfig();

        for (let i = 0; i < files.length; i++) {
            let fullFilePath = path.join(test_files, files[i]);
            let doc = await vscode.workspace.openTextDocument(fullFilePath);
            const maxLines = 10000;
            let buffer = new Array<string>(maxLines);
            let nbuffer = 0;

            // Set options
            await setDefaultOptions();
            await handleFileOptions(files[i]);
            (<any>provider).updateConfig();

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