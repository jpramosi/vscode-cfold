import * as vscode from 'vscode'
import { FoldingRange, FoldingRangeProvider, ProviderResult, TextDocument } from 'vscode'
import { log } from './logger';
import { globalConfig } from './globalConfig';
const { performance } = require('perf_hooks');

enum EntityType {
    Unknown,
    Class,
    Comment,
    CommentQuoteBlock,
    CommentSlashBlock,
    Documentation,
    DocumentationQuoteBlock,
    DocumentationSlashBlock,
    Enum,
    Function,
    Namespace,
    Preprocessor,
    String,
    StringBlock,
    Struct,
    WithinFunction,
    Other,
}

class StringEntityType {
    name: string;
    enum_t: EntityType;

    constructor(p_name: string, p_enum_t: EntityType) {
        this.name = p_name;
        this.enum_t = p_enum_t;
    }
}

class CharInfo {
    line: number;
    column: number;
    flag: number;

    constructor(p_line: number, p_column: number, p_flag: number = 0) {
        this.line = p_line;
        this.column = p_column;
        this.flag = p_flag;
    }
}

class Range {
    startLine: number = 0;
    startCol: number = 0;
    endLine: number = 0;
    endCol: number = 0;
    scope: number = 0;
    dist: number = 0;
    type: EntityType = EntityType.Unknown;
}

export default class ConfigurableFoldingProvider implements FoldingRangeProvider {

    private debug_ = false;

    private classEnable = false;

    private commentQuoteEnable = true;

    //private commentSlashEnable = true;

    private documentationQuoteEnable = true;

    //private documentationSlashEnable = true;

    private enumEnable = false;

    private functionEnable = true;

    private namespaceEnable = false;

    private preprocessorEnable = false;
    private preprocessorIgnoreGuard = true
    private preprocessorMinLines = 0;
    private preprocessorRecursiveDepth = 1;

    private structEnable = false;

    private withinFunctionEnable = false;
    private withinFunctionMinLines = 0;

    private maxElements_ = 500;
    private rangesSearchTerm_ = new Array<StringEntityType>(
        new StringEntityType('namespace', EntityType.Namespace),
        new StringEntityType('class', EntityType.Class),
        new StringEntityType('struct', EntityType.Struct),
        new StringEntityType('enum', EntityType.Enum));

    /** This range contains preprocessor directives. */
    private preprocRanges_ = new Array<Range>(this.maxElements_);
    private npreprocRanges_ = 0;

    /** This range contains literal ranges like comments or string values. */
    private stringRanges_ = new Array<Range>(this.maxElements_);
    private nstringRanges_ = 0;

    /** This range contains only function ranges. */
    private funcRanges_ = new Array<Range>(this.maxElements_);
    private nfuncRanges_ = 0;

    /** This range contains only ranges within functions. */
    private withinFuncRanges_ = new Array<Range>(this.maxElements_);
    private nwithinFuncRanges_ = 0;

    /** This range contains namespaces, classes, structs, enums */
    private ranges_ = new Array<Range>(this.maxElements_);
    private nranges_ = 0;

    constructor(debug: boolean) {
        this.debug_ = debug;
        for (let i = 0; i < this.maxElements_; i++) {
            this.preprocRanges_[i] = new Range();
            this.stringRanges_[i] = new Range();
            this.ranges_[i] = new Range();
            this.funcRanges_[i] = new Range();
            this.withinFuncRanges_[i] = new Range();
        }
    }

    private inStringBlock(line: number, startCol: number, endCol: number) {
        for (let i = 0; i < this.stringRanges_.length; i++) {
            // Check whether line is within string bounds
            if (line >= this.stringRanges_[i].startLine
                && line <= this.stringRanges_[i].endLine
                // Check whether it is within column bounds
                && startCol >= this.stringRanges_[i].startCol
                && endCol <= this.stringRanges_[i].endCol) {
                return true;
            }
        }
        return false;
    }

    private getIndicesOf(searchStr: string, str: string) {
        var searchStrLen = searchStr.length;
        if (searchStrLen == 0) {
            return [];
        }
        var startIndex = 0, index, indices = [];
        while ((index = str.indexOf(searchStr, startIndex)) > -1) {
            indices.push(index);
            startIndex = index + searchStrLen;
        }
        return indices;
    }

    private isEmptyOrWhitespace(str: string) {
        return str === null || str.match(/^ *$/) !== null;
    }

    private isUpperCase(str: string) {
        return str === str.toUpperCase();
    }

    private updateConfig() {
        if (!this.debug_) {
            this.classEnable = globalConfig.get('class.enable', false);
            this.commentQuoteEnable = globalConfig.get('commentQuote.enable', true);
            //this.commentSlashEnable = globalConfig.get('commentSlash.enable', true);
            this.documentationQuoteEnable = globalConfig.get('documentationQuote.enable', true);
            //this.documentationSlashEnable = globalConfig.get('documentationSlash.enable', true);
            this.enumEnable = globalConfig.get('enum.enable', false);
            this.functionEnable = globalConfig.get('function.enable', true);
            this.namespaceEnable = globalConfig.get('namespace.enable', false);
            this.preprocessorEnable = globalConfig.get('preprocessor.enable', false);
            this.preprocessorIgnoreGuard = globalConfig.get('preprocessor.ignoreGuard', true);
            this.preprocessorMinLines = globalConfig.get('preprocessor.minLines', 0);
            this.preprocessorRecursiveDepth = globalConfig.get('preprocessor.recursiveDepth', 1);
            this.structEnable = globalConfig.get('struct.enable', false);
            this.withinFunctionEnable = globalConfig.get('withinFunction.enable', false);
            this.withinFunctionMinLines = globalConfig.get('withinFunction.minLines', 0);
        }
        else {
            this.classEnable = true;
            this.commentQuoteEnable = true;
            //this.commentSlashEnable = true;
            this.documentationQuoteEnable = true;
            //this.documentationSlashEnable = true;
            this.enumEnable = true;
            this.functionEnable = true;
            this.namespaceEnable = true;
            this.preprocessorEnable = true;
            this.preprocessorIgnoreGuard = true;
            this.preprocessorMinLines = 0;
            this.preprocessorRecursiveDepth = 1;
            this.structEnable = true;
            this.withinFunctionEnable = true;
            this.withinFunctionMinLines = 0;
        }

        // Validate config
        if (this.preprocessorMinLines < 0)
            this.preprocessorMinLines = 0;
        if (this.preprocessorRecursiveDepth < 0)
            this.preprocessorRecursiveDepth = 0;
        if (this.withinFunctionMinLines < 0)
            this.withinFunctionMinLines = 0;
    }

    private reset() {
        for (let i = 0; i < this.preprocRanges_.length; i++) {
            this.preprocRanges_[i].startLine = 0;
            this.preprocRanges_[i].startCol = 0;
            this.preprocRanges_[i].endLine = 0;
            this.preprocRanges_[i].endCol = 0;
            this.preprocRanges_[i].scope = 0;
            this.preprocRanges_[i].dist = 0;
            this.preprocRanges_[i].type = EntityType.Unknown;
        }
        for (let i = 0; i < this.stringRanges_.length; i++) {
            this.stringRanges_[i].startLine = 0;
            this.stringRanges_[i].startCol = 0;
            this.stringRanges_[i].endLine = 0;
            this.stringRanges_[i].endCol = 0;
            this.stringRanges_[i].scope = 0;
            this.stringRanges_[i].dist = 0;
            this.stringRanges_[i].type = EntityType.Unknown;
        }
        for (let i = 0; i < this.funcRanges_.length; i++) {
            this.funcRanges_[i].startLine = 0;
            this.funcRanges_[i].startCol = 0;
            this.funcRanges_[i].endLine = 0;
            this.funcRanges_[i].endCol = 0;
            this.funcRanges_[i].scope = 0;
            this.funcRanges_[i].dist = 0;
            this.funcRanges_[i].type = EntityType.Unknown;
        }
        for (let i = 0; i < this.withinFuncRanges_.length; i++) {
            this.withinFuncRanges_[i].startLine = 0;
            this.withinFuncRanges_[i].startCol = 0;
            this.withinFuncRanges_[i].endLine = 0;
            this.withinFuncRanges_[i].endCol = 0;
            this.withinFuncRanges_[i].scope = 0;
            this.withinFuncRanges_[i].dist = 0;
            this.withinFuncRanges_[i].type = EntityType.Unknown;
        }
        for (let i = 0; i < this.ranges_.length; i++) {
            this.ranges_[i].startLine = 0;
            this.ranges_[i].startCol = 0;
            this.ranges_[i].endLine = 0;
            this.ranges_[i].endCol = 0;
            this.ranges_[i].scope = 0;
            this.ranges_[i].dist = 0;
            this.ranges_[i].type = EntityType.Unknown;
        }
        this.npreprocRanges_ = 0;
        this.nstringRanges_ = 0;
        this.nfuncRanges_ = 0;
        this.nwithinFuncRanges_ = 0;
        this.nranges_ = 0;
    }

    public provideFoldingRanges(document: TextDocument): ProviderResult<FoldingRange[]> {
        log('~~~~~~~~~~~~~~~~~~~~~~~~~~~~~provide~~~~~~~~~~~~~~~~~~~~~~~~~~~~~');
        var t0 = performance.now();

        const editor = vscode.window.activeTextEditor;
        if (editor === undefined && !this.debug_) {
            const zero: FoldingRange[] = [];
            return zero;
        }

        this.updateConfig();
        this.reset();

        let preprocStack = new Array<CharInfo>();
        let docStack = new Array<CharInfo>();
        let rangeStack = new Array<CharInfo>();
        let funcStack = new Array<CharInfo>();

        let startStringBlockLine = -1;
        let startStringBlockCol = -1;

        let funcCandidate = new CharInfo(-1, -1);
        let funcBracketSet = false;
        let funcIsCtor = false;

        let bracketType = EntityType.Unknown;


        // Iterate lines of document
        const lineCount = document.lineCount;
        let line = "";
        for (let i = 0; i < lineCount; i++) {
            line = document.lineAt(i).text;




            ////////////////////////////////////////////////
            /// Handle preprocessor
            ////////////////////////////////////////////////

            if (this.preprocessorEnable) {
                if (line.startsWith('#if')) {
                    log('preproc push: [L' + i + ']' + line);
                    let headerDef = 0;
                    if (this.preprocessorIgnoreGuard
                        && (line.endsWith('_HPP')
                            || line.endsWith('_HH')
                            || line.endsWith('_H')))
                        headerDef = 1;
                    preprocStack.push(new CharInfo(i, 0, headerDef));
                }
                else {
                    let preprocElif = line.startsWith('#elif');
                    let preprocElse = line.startsWith('#else');
                    let preprocEndif = line.startsWith('#endif');
                    if (preprocElif || preprocElse || preprocEndif) {
                        if (preprocStack.length > 0) {
                            let pop = preprocStack.pop() || new CharInfo(0, 0);
                            if (this.npreprocRanges_ < this.maxElements_ && pop.flag !== 1) {
                                let mod = 0;
                                // Shift end to avoid slipping into the scope of #if
                                if (preprocElif || preprocElse)
                                    mod = 1;
                                const idx = this.npreprocRanges_;
                                this.preprocRanges_[idx].startLine = pop.line;
                                this.preprocRanges_[idx].startCol = pop.column;
                                this.preprocRanges_[idx].endLine = i - mod;
                                this.preprocRanges_[idx].endCol = 0;
                                this.preprocRanges_[idx].scope = preprocStack.length;
                                this.preprocRanges_[idx].dist =
                                    (this.preprocRanges_[idx].endLine + mod) - this.preprocRanges_[idx].startLine;
                                this.preprocRanges_[idx].type = EntityType.Preprocessor;
                                log('preproc block add: [L' + pop.line +
                                    '->L' + (i - mod) + '] ' + line);
                                this.npreprocRanges_++;
                            }
                        }
                    }
                    if (preprocElif || preprocElse) {
                        log('preproc else(if) push: [L' + i + ']' + line);
                        preprocStack.push(new CharInfo(i, 0));
                    }
                }
            }




            ////////////////////////////////////////////////
            /// Handle documentation- or comment blocks
            ////////////////////////////////////////////////

            // Push & pop blocks
            {
                let odoc = this.getIndicesOf('/*', line);
                let cdoc = this.getIndicesOf('*/', line);
                for (let j = 0; j < odoc.length; j++) {
                    let isDoc = 0;
                    if (odoc[j] + 2 < line.length && line.charAt(odoc[j] + 2) == '*')
                        isDoc = 1;
                    docStack.push(new CharInfo(i, odoc[j], isDoc))
                }
                for (let j = 0; j < cdoc.length; j++) {
                    if (docStack.length == 0)
                        break;
                    let pop = docStack.pop() || new CharInfo(0, 0);
                    if (this.nstringRanges_ >= this.maxElements_)
                        continue;
                    const idx = this.nstringRanges_;
                    this.stringRanges_[idx].startLine = pop.line;
                    this.stringRanges_[idx].startCol = pop.column;
                    this.stringRanges_[idx].endLine = i;
                    this.stringRanges_[idx].endCol = cdoc[j];
                    this.stringRanges_[idx].scope = 0;
                    this.stringRanges_[idx].dist =
                        this.stringRanges_[idx].endLine - this.stringRanges_[idx].startLine;
                    this.stringRanges_[idx].type =
                        pop.flag == 1 ? EntityType.DocumentationQuoteBlock : EntityType.CommentQuoteBlock;
                    log('doc/comment block add: [L' + pop.line + ':' +
                        this.stringRanges_[idx].startCol +
                        '->L' + i + ':' + this.stringRanges_[idx].endCol + '] [TYPE:'
                        + EntityType[this.stringRanges_[idx].type] + ']');
                    this.nstringRanges_++;
                }
            }




            ////////////////////////////////////////////////
            /// Handle string blocks
            ////////////////////////////////////////////////

            // Check whether the string block ends
            if (startStringBlockLine >= 0) {
                let endStringBlockCol = line.indexOf(')"');
                if (endStringBlockCol !== -1) {
                    if (this.nstringRanges_ < this.maxElements_) {
                        log('stringblock release: [L' + i + ']' + line);
                        const idx = this.nstringRanges_;
                        this.stringRanges_[idx].startLine = startStringBlockLine;
                        this.stringRanges_[idx].startCol = startStringBlockCol;
                        this.stringRanges_[idx].endLine = i;
                        this.stringRanges_[idx].endCol = endStringBlockCol;
                        this.stringRanges_[idx].scope = 0;
                        this.stringRanges_[idx].dist =
                            this.stringRanges_[idx].endLine - this.stringRanges_[idx].startLine;
                        this.stringRanges_[idx].type = EntityType.String;
                        this.nstringRanges_++;
                        startStringBlockLine = -1;
                    }
                }
                else {
                    continue;
                }
            }
            // Check whether it is a start of a string block
            {
                startStringBlockCol = line.indexOf('R"(');
                if (startStringBlockCol !== -1) {
                    let endStringBlockCol = line.indexOf(')"');
                    // Check whether it is on same line
                    if (endStringBlockCol !== -1) {
                        if (this.nstringRanges_ < this.maxElements_) {
                            const idx = this.nstringRanges_;
                            this.stringRanges_[idx].startLine = i;
                            this.stringRanges_[idx].startCol = startStringBlockCol;
                            this.stringRanges_[idx].endLine = i;
                            this.stringRanges_[idx].endCol = endStringBlockCol;
                            this.stringRanges_[idx].scope = 0;
                            this.stringRanges_[idx].dist = 0;
                            this.stringRanges_[idx].type = EntityType.String;
                            log('stringblock single add: [L' + i + ':' +
                                this.stringRanges_[idx].startCol + '->' +
                                this.stringRanges_[idx].endCol + '] ' + line);
                            this.nstringRanges_++;
                        }
                    }
                    else {
                        log('stringblock push: [L' + i + ']' + line);
                        startStringBlockLine = i;
                        continue;
                    }
                }
            }




            ////////////////////////////////////////////////
            /// Handle single line documentation or comments
            ////////////////////////////////////////////////

            // Gather comments from current line
            {
                let odoc = line.indexOf('//');
                if (odoc !== -1) {
                    let isDoc = 0;
                    if (odoc + 2 < line.length && line.charAt(odoc + 2) == '/')
                        isDoc = 1;
                    if (this.nstringRanges_ < this.maxElements_) {
                        const idx = this.nstringRanges_;
                        this.stringRanges_[idx].startLine = i;
                        this.stringRanges_[idx].startCol = odoc;
                        this.stringRanges_[idx].endLine = i;
                        this.stringRanges_[idx].endCol = line.length;
                        this.stringRanges_[idx].scope = 0;
                        this.stringRanges_[idx].dist =
                            this.stringRanges_[idx].endLine - this.stringRanges_[idx].startLine;
                        this.stringRanges_[idx].type =
                            isDoc ? EntityType.Documentation : EntityType.Comment;
                        log('doc/comment single add: [L' + i + ':' +
                            this.stringRanges_[idx].startCol + '->' +
                            this.stringRanges_[idx].endCol + '] [TYPE:'
                            + EntityType[this.stringRanges_[idx].type] + ']' + line);
                        this.nstringRanges_++;
                    }
                }
            }




            ////////////////////////////////////////////////
            /// Handle string values
            ////////////////////////////////////////////////

            // Gather string value sets from current line
            {
                // Search for all quote occurrences
                let startIndex = 0;
                let index = 0;
                let quotes = [];
                const quoteStr = '"';
                const nquoteStr = quoteStr.length;
                while ((index = line.indexOf(quoteStr, startIndex)) > -1) {
                    if (index == 0 || line.charAt(index - 1) != '\\')
                        quotes.push(index);
                    startIndex = index + nquoteStr;
                }
                // Add quote sets
                if (quotes.length > 1) {
                    for (let j = 0; j < quotes.length; j = j + 2) {
                        if (this.nstringRanges_ < this.maxElements_) {
                            const idx = this.nstringRanges_;
                            this.stringRanges_[idx].startLine = i;
                            this.stringRanges_[idx].startCol = quotes[j];
                            this.stringRanges_[idx].endLine = i;
                            this.stringRanges_[idx].endCol = quotes[j + 1];
                            this.stringRanges_[idx].scope = 0;
                            this.stringRanges_[idx].dist = 0;
                            this.stringRanges_[idx].type = EntityType.String;
                            log('string add: [L' + i + ':' +
                                this.stringRanges_[idx].startCol + '->' +
                                this.stringRanges_[idx].endCol + '] ' + line);
                            this.nstringRanges_++;
                        }
                    }
                }
            }




            ////////////////////////////////////////////////
            /// Handle functions
            ////////////////////////////////////////////////

            // Check whether it is a function
            if (this.functionEnable && funcCandidate.line !== -1) {

                // Reset if it ends with a semicolon
                if (!funcBracketSet && line.includes(';')) {
                    log('func not valid [' + i + '] ' + line);
                    funcCandidate.line = -1;
                    funcCandidate.column = -1;
                    funcIsCtor = false;
                    funcStack = new Array<CharInfo>();
                    continue;
                }

                // Check whether the function is a constructor
                if (!funcBracketSet && line.includes(' :')) {
                    funcIsCtor = true;
                }

                // Push open bracket
                let obracket = this.getIndicesOf('{', line);
                if (obracket.length > 0) {
                    funcBracketSet = true;
                    for (let j = 0; j < obracket.length; j++) {
                        log('func push { [' + i + ']')
                        funcStack.push(new CharInfo(i, obracket[j]));
                    }
                }

                /* // Handle brackets within function
                if (funcBracketSet && this.withinFunctionEnable)
                {

                } */

                // Pop close bracket
                let cbracket = this.getIndicesOf('}', line);
                if (cbracket.length > 0) {
                    funcBracketSet = true;
                    for (let j = 0; j < cbracket.length; j++) {
                        if (funcStack.length > 0) {
                            log('func pop  } [' + i + ']')
                            let pop = funcStack.pop() || new CharInfo(0, 0);
                            // Check whether it has the same idention
                            if ((cbracket[j] === funcCandidate.column)
                                || (funcIsCtor
                                    && funcStack.length === 0
                                    && this.isEmptyOrWhitespace(line))) {
                                if (this.nfuncRanges_ < this.maxElements_) {
                                    log('func add [' + pop.line + '-' + i + ']')
                                    // Add range
                                    const idx = this.nfuncRanges_;
                                    this.funcRanges_[idx].startLine = pop.line;
                                    this.funcRanges_[idx].startCol = pop.column;
                                    this.funcRanges_[idx].endLine = i;
                                    this.funcRanges_[idx].endCol = cbracket[j];
                                    this.funcRanges_[idx].scope = 0;
                                    this.funcRanges_[idx].dist =
                                        this.funcRanges_[idx].endLine - this.funcRanges_[idx].startLine;
                                    this.funcRanges_[idx].type = EntityType.Function;
                                    this.nfuncRanges_++;
                                }
                                // Reset
                                funcCandidate.line = -1;
                                funcCandidate.column = -1;
                                funcBracketSet = false;
                                funcIsCtor = false;
                                funcStack = new Array<CharInfo>();
                            }
                            // Handle brackets within function
                            else if (this.withinFunctionEnable
                                && this.nwithinFuncRanges_ < this.maxElements_
                                && cbracket[j] !== -1
                                && cbracket[j] >= funcCandidate.column
                                && pop.column >= funcCandidate.column
                                && pop.line !== i
                                && i - pop.line >= this.withinFunctionMinLines
                                && !this.inStringBlock(pop.line, pop.column, pop.column + 1)
                                && !this.inStringBlock(i, cbracket[j], cbracket[j] + 1)) {
                                log('within func add [' + pop.line + '-' + i + ']');
                                // Add range
                                const idx = this.nwithinFuncRanges_;
                                this.withinFuncRanges_[idx].startLine = pop.line;
                                this.withinFuncRanges_[idx].startCol = pop.column;
                                this.withinFuncRanges_[idx].endLine = i;
                                this.withinFuncRanges_[idx].endCol = cbracket[j];
                                this.withinFuncRanges_[idx].scope = 0;
                                this.withinFuncRanges_[idx].dist =
                                    this.withinFuncRanges_[idx].endLine - this.withinFuncRanges_[idx].startLine;
                                this.withinFuncRanges_[idx].type = EntityType.WithinFunction;
                                this.nwithinFuncRanges_++;
                            }
                        }
                    }
                }
                continue;
            }

            // Check whether it is a start of a function
            if (this.functionEnable && funcCandidate.line === -1 && docStack.length === 0) {
                let obrace = line.indexOf('(');
                if (obrace !== -1 && !line.includes(';')) {
                    let objects = line.match(/\S+/g) || [];
                    let inString = false;
                    // Check if found block with open brace is in string block
                    let objIdx = -1;
                    let objColumn = -1;
                    for (let j = 0; j < objects.length; j++) {
                        objColumn = objects[j].indexOf('(');
                        if (objColumn !== -1) {
                            objIdx = j;
                            let startObjIdx = line.indexOf(objects[j]);
                            let endObjIdx = startObjIdx + objects[j].length;
                            if (this.inStringBlock(i, startObjIdx, endObjIdx)) {
                                log('func in string [' + i + ':' + startObjIdx + '-' + endObjIdx + '] ' + line);
                                inString = true;
                            }
                            break;
                        }
                    }
                    if (objects.length > 0 && !inString // chc > 1
                        // This shouldn't happen anyway
                        && objIdx !== -1 && objColumn !== -1) {
                        // Check whether it is a macro function call
                        if (this.isUpperCase(objects[objIdx].substr(0, objColumn))) {
                            log('func is macro [' + i + '] ' + line);
                            continue;
                        }
                        let funcLine = i;
                        let funcLineText = line;
                        let braceStack = 0;

                        // Iterate til to the end of the curly brace
                        for (i = i; i < lineCount; i++) {
                            line = document.lineAt(i).text;
                            braceStack = braceStack + (line.split('(').length - 1);
                            braceStack = braceStack - (line.split(')').length - 1);
                            if (braceStack > 0)
                                continue;
                            else
                                break;
                        }

                        // Check again for semicolon at the end of curly brace
                        if (line.includes(';'))
                            continue;

                        // Skip one-liner
                        let bopen = this.getIndicesOf('{', line);
                        let bclose = this.getIndicesOf('}', line);
                        if (bopen.length > 0 && bopen.length === bclose.length)
                            continue;

                        // Probably in function
                        funcCandidate.line = funcLine;
                        funcCandidate.column = funcLineText.indexOf(objects[0]);
                        log('func candidate detect [' + funcCandidate.line + ':' + funcCandidate.column + '] ' + funcLineText);

                        // Push open brackets
                        if (bopen.length !== -1) {
                            funcBracketSet = true;
                            for (let j = 0; j < bopen.length; j++) {
                                log('_func push { [' + i + ']')
                                funcStack.push(new CharInfo(i, bopen[j]));
                            }
                        }
                        // Pop close brackets
                        if (bopen.length !== -1) {
                            funcBracketSet = true;
                            for (let j = 0; j < bclose.length; j++) {
                                if (funcStack.length == 0)
                                    break;
                                log('_func pop  } [' + i + ']')
                                funcStack.pop() || new CharInfo(0, 0);
                            }
                        }

                        // Check whether the function is a constructor
                        if (line.includes(' :')) {
                            funcIsCtor = true;
                        }
                        continue;
                    }
                }
            }




            // After this line non-functions brackets are available.
            // To correctly process brackets, it needs to push & pop them all
            {
                // Set identifier for the next bracket
                for (let term of this.rangesSearchTerm_) {
                    let idx = line.indexOf(term.name);
                    if (idx !== -1
                        && !this.inStringBlock(i, idx, idx + term.name.length)) {
                        bracketType = term.enum_t;
                        break;
                    }
                }
                // Invalidate identifier if semicolon is found
                if (bracketType !== EntityType.Unknown) {
                    if (line.includes(';')) {
                        bracketType = EntityType.Unknown;
                    }
                }
            }




            ////////////////////////////////////////////////
            /// Handle namespaces, structs, classes, enums
            ////////////////////////////////////////////////

            {
                let obracket = this.getIndicesOf('{', line);
                let cbracket = this.getIndicesOf('}', line);
                for (let j = 0; j < obracket.length; j++) {
                    log('range push { [' + i + '] [TYPE:'
                        + EntityType[bracketType] + ']')
                    rangeStack.push(new CharInfo(i, obracket[j], bracketType))
                }
                for (let j = 0; j < cbracket.length; j++) {
                    if (rangeStack.length == 0)
                        break;
                    let pop = rangeStack.pop() || new CharInfo(0, 0);
                    if (this.nranges_ >= this.maxElements_)
                        continue;
                    const idx = this.nranges_;
                    this.ranges_[idx].startLine = pop.line;
                    this.ranges_[idx].startCol = pop.column;
                    this.ranges_[idx].endLine = i;
                    this.ranges_[idx].endCol = cbracket[j];
                    this.ranges_[idx].scope = 0;
                    this.ranges_[idx].dist =
                        this.ranges_[idx].endLine - this.ranges_[idx].startLine;
                    this.ranges_[idx].type = pop.flag;
                    log('range add: [L' + pop.line + ':' +
                        this.ranges_[idx].startCol +
                        '->L' + i + ':' + this.ranges_[idx].endCol + '] [TYPE:'
                        + EntityType[this.ranges_[idx].type] + ']');
                    this.nranges_++;
                }
            }
        }




        ////////////////////////////////////////////////
        /// Add the found ranges to a new folding range
        ////////////////////////////////////////////////

        // Todo maybe store them and re-use later
        const foldingRanges = new Array<FoldingRange>();
        if (this.preprocessorEnable) {
            for (let i = 0; i < this.npreprocRanges_; i++) {
                if (this.preprocRanges_[i].scope <= this.preprocessorRecursiveDepth
                    && this.preprocRanges_[i].dist >= this.preprocessorMinLines)
                    foldingRanges.push(
                        new FoldingRange(this.preprocRanges_[i].startLine, this.preprocRanges_[i].endLine));
            }
        }
        for (let i = 0; i < this.nranges_; i++) {
            if ((this.namespaceEnable && this.ranges_[i].type === EntityType.Namespace)
                || (this.classEnable && this.ranges_[i].type === EntityType.Class)
                || (this.structEnable && this.ranges_[i].type === EntityType.Struct)
                || (this.enumEnable && this.ranges_[i].type === EntityType.Enum))
                foldingRanges.push(
                    new FoldingRange(this.ranges_[i].startLine, this.ranges_[i].endLine));
        }
        for (let i = 0; i < this.nstringRanges_; i++) {
            if ((this.documentationQuoteEnable && this.stringRanges_[i].type === EntityType.DocumentationQuoteBlock)
                || (this.commentQuoteEnable && this.stringRanges_[i].type === EntityType.CommentQuoteBlock))
                foldingRanges.push(
                    new FoldingRange(this.stringRanges_[i].startLine, this.stringRanges_[i].endLine));
        }
        for (let i = 0; i < this.nfuncRanges_; i++) {
            foldingRanges.push(
                new FoldingRange(this.funcRanges_[i].startLine, this.funcRanges_[i].endLine));
        }
        for (let i = 0; i < this.nwithinFuncRanges_; i++) {
            foldingRanges.push(
                new FoldingRange(this.withinFuncRanges_[i].startLine, this.withinFuncRanges_[i].endLine));
        }




        var t1 = performance.now();
        log('finished in ' + lineCount + ' lines in ' + (t1 - t0) + 'ms')
        return foldingRanges;
    }

    public async foldAll() {
        await vscode.commands.executeCommand('editor.foldAll');
    }

    public async foldDocComments() {
        if (vscode.window.activeTextEditor === undefined
            || !vscode.window.activeTextEditor.selection.isEmpty)
            return;
        let lines: number[] = [];

        for (let i = 0; i < this.nstringRanges_; i++) {
            if (this.stringRanges_[i].type === EntityType.DocumentationQuoteBlock
                || this.stringRanges_[i].type === EntityType.CommentQuoteBlock) {
                //log('foldDocComments: [L' + this.stringRanges_[i].startLine + "] [TYPE:"
                //    + EntityType[this.stringRanges_[i].type] + "]");
                lines.push(this.stringRanges_[i].startLine);
            }
        }

        if (lines.length > 1)
            await vscode.commands.executeCommand("editor.fold", { levels: 1, direction: 'up', selectionLines: lines });
    }

    public async foldAroundCursor() {
        if (vscode.window.activeTextEditor === undefined
            || !vscode.window.activeTextEditor.selection.isEmpty)
            return;
        let lines: number[] = [];
        let cursorPos = vscode.window.activeTextEditor.selection.active;

        if (this.preprocessorEnable) {
            for (let i = 0; i < this.npreprocRanges_; i++) {
                if (this.preprocRanges_[i].scope <= this.preprocessorRecursiveDepth
                    && this.preprocRanges_[i].dist >= this.preprocessorMinLines)
                    if (!(cursorPos.line >= this.preprocRanges_[i].startLine && cursorPos.line <= this.preprocRanges_[i].endLine)) {
                        //log('foldAroundCursor->preprocRanges_: [L' + this.preprocRanges_[i].startLine + "] [TYPE:"
                        //    + EntityType[this.preprocRanges_[i].type] + "]");
                        lines.push(this.preprocRanges_[i].startLine);
                    }
            }
        }
        for (let i = 0; i < this.nranges_; i++) {
            if ((this.namespaceEnable && this.ranges_[i].type === EntityType.Namespace)
                || (this.classEnable && this.ranges_[i].type === EntityType.Class)
                || (this.structEnable && this.ranges_[i].type === EntityType.Struct)
                || (this.enumEnable && this.ranges_[i].type === EntityType.Enum))
                if (!(cursorPos.line >= this.ranges_[i].startLine && cursorPos.line <= this.ranges_[i].endLine)) {
                    //log('foldAroundCursor->ranges_: [L' + this.ranges_[i].startLine + "] [TYPE:"
                    //    + EntityType[this.ranges_[i].type] + "]");
                    lines.push(this.ranges_[i].startLine);
                }
        }
        for (let i = 0; i < this.nstringRanges_; i++) {
            if ((this.documentationQuoteEnable && this.stringRanges_[i].type === EntityType.DocumentationQuoteBlock)
                || (this.commentQuoteEnable && this.stringRanges_[i].type === EntityType.CommentQuoteBlock))
                if (!(cursorPos.line >= this.stringRanges_[i].startLine && cursorPos.line <= this.stringRanges_[i].endLine)) {
                    //log('foldAroundCursor->stringRanges_: [L' + this.stringRanges_[i].startLine + "] [TYPE:"
                    //    + EntityType[this.stringRanges_[i].type] + "]");
                    lines.push(this.stringRanges_[i].startLine);
                }
        }
        for (let i = 0; i < this.nfuncRanges_; i++) {
            if (!(cursorPos.line >= this.funcRanges_[i].startLine && cursorPos.line <= this.funcRanges_[i].endLine)) {
                //log('foldAroundCursor->funcRanges_: [L' + this.funcRanges_[i].startLine + "] [TYPE:"
                //    + EntityType[this.funcRanges_[i].type] + "]");
                lines.push(this.funcRanges_[i].startLine);
            }
        }
        for (let i = 0; i < this.nwithinFuncRanges_; i++) {
            if (!(cursorPos.line >= this.withinFuncRanges_[i].startLine && cursorPos.line <= this.withinFuncRanges_[i].endLine)) {
                //log('foldAroundCursor->withinFuncRanges_: [L' + this.withinFuncRanges_[i].startLine + "] [TYPE:"
                //    + EntityType[this.withinFuncRanges_[i].type] + "]");
                lines.push(this.withinFuncRanges_[i].startLine);
            }
        }

        if (lines.length > 1)
            await vscode.commands.executeCommand("editor.fold", { levels: 1, direction: 'up', selectionLines: lines });
    }

    public async foldFunction() {
        if (vscode.window.activeTextEditor === undefined
            || !vscode.window.activeTextEditor.selection.isEmpty)
            return;
        let lines: number[] = [];

        for (let i = 0; i < this.nfuncRanges_; i++) {
            lines.push(this.funcRanges_[i].startLine);
        }
        for (let i = 0; i < this.nwithinFuncRanges_; i++) {
            lines.push(this.withinFuncRanges_[i].startLine);
        }

        if (lines.length > 1)
            await vscode.commands.executeCommand("editor.fold", { levels: 1, direction: 'up', selectionLines: lines });
    }

    public async foldFunctionClassStructEnum() {
        if (vscode.window.activeTextEditor === undefined
            || !vscode.window.activeTextEditor.selection.isEmpty)
            return;
        let lines: number[] = [];

        for (let i = 0; i < this.nfuncRanges_; i++) {
            lines.push(this.funcRanges_[i].startLine);
        }
        for (let i = 0; i < this.nwithinFuncRanges_; i++) {
            lines.push(this.withinFuncRanges_[i].startLine);
        }
        for (let i = 0; i < this.nranges_; i++) {
            if ((this.namespaceEnable && this.ranges_[i].type === EntityType.Namespace)
                || (this.classEnable && this.ranges_[i].type === EntityType.Class)
                || (this.structEnable && this.ranges_[i].type === EntityType.Struct)
                || (this.enumEnable && this.ranges_[i].type === EntityType.Enum))
                lines.push(this.ranges_[i].startLine);
        }

        if (lines.length > 1)
            await vscode.commands.executeCommand("editor.fold", { levels: 1, direction: 'up', selectionLines: lines });
    }
}