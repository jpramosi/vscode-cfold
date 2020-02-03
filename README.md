Cfold
===========

![MIT](https://img.shields.io/badge/license-MIT-blue.svg)

Cfold is a visual studio code extension that provides advanced folding capability with function, class, struct, preprocessor, documentation- & comment blocks particular designed for C & C++ based on the stack principle.

<br>

## Motivation

I have created this extension for myself because there wasn't anything to control folding on a good way.<br>
Other extensions allowed only specific regex or keywords, other folded namespaces and header guards,<br>
other messed up the preprocessor directives and the list goes on.<br>
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
| cfold.foldFunctionClassStruct     | Fold all functions, classes & structs |
| cfold.toggleLog                   | Toggle log |

<br>

## Available configuration

These confiugrations can be set in the common vscode settings menu:

| Config                            | Default   |Description                               |
|-----------------------------------|-----------|------------------------------------------|
| cfold.provide.preprocessor        | true      | Provide fold controls for preprocessor directives |
| cfold.provide.namespace           | false     | Provide fold controls for namespace |
| cfold.provide.class               | false     | Provide fold controls for class |
| cfold.provide.struct              | false     | Provide fold controls for struct |
| cfold.provide.function            | true      | Provide fold controls for function |
| cfold.provide.documentation       | true      | Provide fold controls for documentation block |
| cfold.provide.comments            | true      | Provide fold controls for comment block |
| cfold.preprocessor.depth          | 1         | Sets the recurse level for providing fold controls for proprocessor directives |
| cfold.preprocessor.ignoreGuard    | true      | Disable fold controls for header guards |
| cfold.preprocessor.minLines       | 0         | Minimum lines for providing fold controls for proprocessor directives |
| cfold.language.c                  | true      | Enable Cfold for c language |
| cfold.language.cpp                | true      | Enable Cfold for c++ language |

Configurations which start with 'cfold.provide*' only enables folding controls on the left sidebar.<br>
That means if command 'cfold.foldAll' is executed, it will just folds the provided controls.<br>
With this behavior it can be further customized.

<br>

## Issues

If you encounter a problem with a file, you may open an issue with the file content<br>
or chat with me privately on https://gitter.im/reapler/ with your github account.<br>
If the folding doesn't seem to change at all, you may need to investigate your other extensions<br>
for possible folding range overriding. Often it can be disabled.<br>
Please keep in mind that uncommon code styles will likely not be supported.<br>