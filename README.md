Cfold
===========

![MIT](https://img.shields.io/badge/license-MIT-blue.svg)

Cfold is a visual studio code extension that provides advanced folding capabilities with function, class, struct, preprocessor, documentation- & comment blocks particular designed for C & C++ based on the stack principle.

<br>

## Motivation

I have created this extension for myself because there wasn't anything to control folding on a good specific way.<br>
So, here it is. Cfold.<br>
However i'm not an everyday type-/javascript developer, if you find something fishy in the code,<br>
please open an issue or pullrequest.

<br>

## Available commands

These commands can be used & mapped with shortcuts:

| Command                           |Description                               |
|-----------------------------------|------------------------------------------|
| cfold.foldAll                     | Fold all provided fold controls |
| cfold.foldDocComments             | Fold all comments & documentation blocks |
| cfold.foldAroundCursor            | Fold all controls around cursor |
| cfold.foldFunction                | Fold all functions |
| cfold.foldFunctionClassStructEnum | Fold all functions, classes, structs & enums |
| cfold.toggleLog                   | Toggle log |

<br>

## Available configuration

These confiugrations can be set in the common vscode settings menu:

| Config                            | Default   |Description                               |
|-----------------------------------|-----------|------------------------------------------|
| cfold.caseLabel.enable            | false     | Enable fold controls for case labels within a switch |
| cfold.caseLabel.minLines          | 0         | Minimum lines for providing fold controls for case labels within a switch |
| cfold.class.enable                | false     | Enable fold controls for class |
| cfold.commentQuote.enable         | true      | Enable fold controls for quoted comment block |
| cfold.documentationQuote.enable   | true      | Enable fold controls for quoted documentation block |
| cfold.enum.enable                 | false     | Enable fold controls for enum |
| cfold.function.enable             | true      | Enable fold controls for function |
| cfold.namespace.enable            | false     | Enable fold controls for namespace |
| cfold.preprocessor.enable         | true      | Enable fold controls for preprocessor directives |
| cfold.preprocessor.ignoreGuard    | true      | Disable fold controls for header guards |
| cfold.preprocessor.minLines       | 0         | Minimum lines for providing fold controls for preprocessor directives |
| cfold.preprocessor.recursiveDepth | 1         | Sets the recurse level for providing fold controls for preprocessor directives |
| cfold.struct.enable               | false     | Enable fold controls for struct |
| cfold.withinFunction.enable       | false     | Enable fold controls within functions |
| cfold.withinFunction.minLines     | 0         | Minimum lines for providing fold controls within functions |
| cfold.language.c                  | true      | Enable Cfold for c language |
| cfold.language.cpp                | true      | Enable Cfold for c++ language |

Configurations which start with 'cfold.xxxxxxxx.enable' only enables folding controls on the left sidebar.<br>
That means if command 'cfold.foldAll' is executed, it will just folds the provided controls.<br>
With this behavior it can be further customized.

<br>

## Issues

If you encounter a problem with a file, you may open an issue with the file content<br>
or chat with me privately on https://gitter.im/reapler/ with your github account.<br>
If the folding doesn't seem to change at all, you may need to investigate your other extensions<br>
for possible folding range overriding. Often it can be disabled.<br>
Please keep in mind that uncommon code styles will likely not be supported.<br>

<br>

## Thanks

Thanks to [@daiyam](https://github.com/daiyam) (https://github.com/zokugun/vscode-explicit-folding) for providing a good foundation for Cfold.<br>