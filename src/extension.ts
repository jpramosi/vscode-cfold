import * as vscode from 'vscode'
const pkg = require('../package.json')

import FoldingProvider from './foldingProvider'
import { g_logChannel, log, toggleLog } from './logger';
import { globalConfig, updateConfig } from './globalConfig';

const VERSION_ID = 'cfoldVersion'
let $disposable: vscode.Disposable | null = null;

function setup(context: vscode.ExtensionContext) {
	if ($disposable !== null) {
		$disposable.dispose();
	}


    // Get configuration & set disposable
    updateConfig();
    const subscriptions: vscode.Disposable[] = [];
    var languages = new Array<string>();
    if (globalConfig.get('language.c', true))
        languages.push('c');
    if (globalConfig.get('language.cpp', true))
        languages.push('cpp');
    if (globalConfig.get('language.csharp', true))
        languages.push('csharp');


    // Register folding providers for each language
    let provider = new FoldingProvider(false);
    let disposable;
    log('cfold initialize');
    subscriptions.push(disposable = g_logChannel);
    context.subscriptions.push(disposable);
    for (let name of languages) {

        subscriptions.push(disposable = vscode.languages.registerFoldingRangeProvider({ language: name, scheme: 'file' }, provider));
        context.subscriptions.push(disposable);

        subscriptions.push(disposable = vscode.languages.registerFoldingRangeProvider({ language: name, scheme: 'untitled' }, provider));
        context.subscriptions.push(disposable);

        log('register folding provider for language \'' + name + '\'');
    }


    // Register commands
    context.subscriptions.push(vscode.commands.registerCommand("cfold.toggleLog", toggleLog));
    context.subscriptions.push(vscode.commands.registerCommand("cfold.foldAll", provider.foldAll, provider));
    context.subscriptions.push(vscode.commands.registerCommand("cfold.foldDocComments", provider.foldDocComments, provider));
    context.subscriptions.push(vscode.commands.registerCommand("cfold.foldAroundCursor", provider.foldAroundCursor, provider));
    context.subscriptions.push(vscode.commands.registerCommand("cfold.foldFunction", provider.foldFunction, provider));
    context.subscriptions.push(vscode.commands.registerCommand("cfold.foldFunctionClassStructEnum", provider.foldFunctionClassStructEnum, provider));


    // Listen to config changes
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(e => {

        if (e.affectsConfiguration('cfold')) {
            updateConfig();
        }
    }));


	$disposable = vscode.Disposable.from(...subscriptions);
}

async function showWhatsNewMessage(version: string) {
    const actions: vscode.MessageItem[] = [{
        title: 'Homepage'
    }, {
        title: 'Release Notes'
    }];

    const result = await vscode.window.showInformationMessage(
        `Cfold has been updated to ${version}.`,
        ...actions
    );

    if (result != null) {
        if (result === actions[0]) {
            await vscode.commands.executeCommand(
                'vscode.open',
                vscode.Uri.parse(`${pkg.homepage}`)
            );
        }
        else if (result === actions[1]) {
            await vscode.commands.executeCommand(
                'vscode.open',
                vscode.Uri.parse(`${pkg.homepage}/blob/master/CHANGELOG.md`)
            );
        }
    }
}

export async function activate(context: vscode.ExtensionContext) {
    const previousVersion = context.globalState.get<string>(VERSION_ID);
    const currentVersion = pkg.version;
    if (previousVersion === undefined || currentVersion !== previousVersion) {
        await showWhatsNewMessage(currentVersion);
        context.globalState.update(VERSION_ID, currentVersion);
    }

	setup(context);
	vscode.workspace.onDidChangeConfiguration(event => {
		if(event.affectsConfiguration('folding')) {
			setup(context);
		}
	});
};