import * as vscode from 'vscode'

export let g_logChannel: vscode.OutputChannel;
export let logEnable = false;

function getLogChannel() {
    if (g_logChannel === undefined) {
        g_logChannel = vscode.window.createOutputChannel('cfold');
        if (logEnable)
            g_logChannel.show();
    }
    return g_logChannel;
}

export function toggleLog() {
    logEnable = !logEnable;
}

export function logError(error: any) {
    if (logEnable)
        getLogChannel().appendLine(`[${getTimeAndms()}][Error] ${error.toString()}`.replace(/(\r\n|\n|\r)/gm, ''));
}

export function log(message: string) {
    if (logEnable)
        getLogChannel().appendLine(`[${getTimeAndms()}][Info] ${message}`);
}

export function logForce(message: string) {
    getLogChannel().appendLine(`[${getTimeAndms()}][Info] ${message}`);
}

function getTimeAndms(): string {
    const time = new Date();
    return ('0' + time.getHours()).slice(-2) + ':' +
        ('0' + time.getMinutes()).slice(-2) + ':' +
        ('0' + time.getSeconds()).slice(-2) + '.' +
        ('00' + time.getMilliseconds()).slice(-3);
}