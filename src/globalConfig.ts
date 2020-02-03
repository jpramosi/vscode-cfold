import * as vscode from 'vscode'

export let globalConfig: vscode.WorkspaceConfiguration;

export function updateConfig() {
    globalConfig = vscode.workspace.getConfiguration('cfold');
}