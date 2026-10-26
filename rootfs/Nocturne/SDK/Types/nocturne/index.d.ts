/**
 * Type declarations for the built-in "nocturne" module of js, the
 * JavaScript runtime of Nocturne OS.
 *
 *     import { readText, writeText } from "nocturne";
 *
 * Scripts run by js can always import "nocturne". These declarations live in
 * C:\Nocturne\SDK\Types\nocturne, and TypeScript (tsc) finds them through the
 * type root C:\Nocturne\SDK\Types: "js file.ts" passes it to tsc, and a
 * project made by "js --init" names it in tsconfig.json:
 *
 *     "typeRoots": ["C:\\Nocturne\\SDK\\Types"], "types": ["nocturne"]
 *
 * The `import.meta` additions at the end of this file (filename, dirname, main)
 * are global augmentations. They apply only while this package is part of the
 * program: import "nocturne" somewhere, or add "nocturne" to
 * `compilerOptions.types` in tsconfig.json.
 */

declare namespace nocturne {
    // ---- The app and its settings -------------------------------------

    /** The arguments after the script name. */
    const args: readonly string[];

    /** Ends the app at once. Queued output is flushed first. `code` defaults to 0. */
    function exit(code?: number): never;

    /** The app's current folder, as a complete path ("C:\Users\User"). */
    function cwd(): string;

    /** Changes the current folder. A relative path is resolved against the current folder. */
    function chdir(path: string): void;

    /**
     * Returns a launch setting, or undefined when it is not set. Every app gets FOLDER (its current
     * folder), USERFOLDER (the user's folder, C:\Users\<name>) and TEMP (the user's temporary folder,
     * emptied at every start).
     */
    function getSetting(name: string): string | undefined;

    /** Sets a launch setting for this app and for the apps it runs. */
    function setSetting(name: string, value: string): void;

    /** Removes a launch setting for this app and for the apps it runs. */
    function removeSetting(name: string): void;

    // ---- Documents and folders ----------------------------------------

    /** What an entry in a folder is. */
    type EntryKind = "document" | "folder";

    /** Facts about one document or folder. */
    interface EntryInfo {
        /** The last part of the path. */
        name: string;
        /** The complete path. */
        path: string;
        kind: EntryKind;
        /** Size in bytes. */
        size: number;
        /** When the entry was last changed. */
        modified: Date;
    }

    /** Options of {@link createFolder}. */
    interface CreateFolderOptions {
        /** Also create missing parent folders, and do not fail when the folder already exists. */
        recursive?: boolean | undefined;
    }

    /** Options of {@link remove}. */
    interface RemoveOptions {
        /** Remove a folder together with everything in it. */
        recursive?: boolean | undefined;
    }

    /** Returns facts about an entry, or null when nothing has that name. Other failures throw a {@link NocturneError}. */
    function info(path: string): EntryInfo | null;

    /** Tells whether an entry with that name exists. */
    function exists(path: string): boolean;

    /** Returns the entries of a folder. Defaults to the current folder. */
    function list(path?: string): EntryInfo[];

    /** Reads a whole document as UTF-8 text. */
    function readText(path: string): string;

    /** Reads a whole document as bytes. */
    function readBytes(path: string): Uint8Array;

    /** Replaces the whole document atomically, creating it when needed. */
    function writeText(path: string, text: string): void;

    /** Replaces the whole document with these bytes atomically, creating it when needed. */
    function writeBytes(path: string, data: ArrayBuffer | ArrayBufferView): void;

    /** Adds text to the end of a document, creating it when needed. */
    function appendText(path: string, text: string): void;

    /** Adds bytes to the end of a document, creating it when needed. */
    function appendBytes(path: string, data: ArrayBuffer | ArrayBufferView): void;

    /** Creates a folder. With `recursive`, also creates the parents and succeeds when the folder already exists. */
    function createFolder(path: string, options?: CreateFolderOptions): void;

    /** Removes a document or an empty folder. With `recursive`, a folder's contents are removed too. */
    function remove(path: string, options?: RemoveOptions): void;

    /** Renames or moves an entry. Never replaces an existing entry: throws code "EXISTS" instead. */
    function move(from: string, to: string): void;

    /** Copies a document. An existing document at `to` is replaced. */
    function copy(from: string, to: string): void;

    /** Returns the complete path, with "." and ".." resolved, relative to {@link cwd}. */
    function resolvePath(path: string): string;

    // ---- Terminal input and output ------------------------------------

    /** Writes to standard output. No newline is added. */
    function write(data: string | ArrayBuffer | ArrayBufferView): void;

    /** Writes to standard error. No newline is added. */
    function writeError(data: string | ArrayBuffer | ArrayBufferView): void;

    /** Reads one line from standard input (with line editing in a terminal), without the line break. Returns null at the end of input. */
    function readLine(prompt?: string): string | null;

    /** Reads the rest of standard input. */
    function readAll(): string;

    /** Tells whether a stream is a terminal. `stream` defaults to "output". */
    function isTerminal(stream?: "input" | "output" | "error"): boolean;

    // ---- Other apps and time ------------------------------------------

    /**
     * Runs another app with this terminal, waits for it and returns its exit status.
     * The app is found like the shell finds it: by name in C:\Nocturne\System, then in each
     * folder of C:\Programs, or by a path.
     */
    function run(program: string, args?: readonly string[]): number;

    /** Resolves after `ms` milliseconds. */
    function sleep(ms: number): Promise<void>;

    // ---- Errors -------------------------------------------------------

    /** Why a call failed. */
    type ErrorCode =
        | "NOT_FOUND"
        | "EXISTS"
        | "DENIED"
        | "NO_MEMORY"
        | "INVALID"
        | "WRONG_KIND"
        | "NO_HANDLE"
        | "BUSY"
        | "NOT_READY"
        | "CLOSED"
        | "TIMEOUT"
        | "CANCELLED"
        | "TOO_LARGE"
        | "NOT_EMPTY"
        | "IO"
        | "UNSUPPORTED"
        | "BAD_ADDRESS"
        | "STOPPED"
        | "NOT_FOLDER"
        | "IS_FOLDER"
        | "READ_ONLY"
        | "NO_SPACE"
        | "NOT_PROGRAM"
        | "NAME_TOO_LONG"
        | "NO_APP"
        | "TOO_MANY_ARGUMENTS"
        | "CONNECTION_RESET"
        | "CONNECTION_REFUSED"
        | "ADDRESS_IN_USE"
        | "NETWORK_DOWN"
        | "NOT_CONNECTED"
        | "UNREACHABLE"
        | "PROTOCOL"
        | "UNKNOWN";

    /** What the functions of this module throw when the system reports a failure. */
    interface NocturneError extends Error {
        /** The reason, for example "NOT_FOUND". */
        code: ErrorCode;
        /** The negative system result. */
        result: number;
        /** The path involved, when there is one. */
        path?: string;
    }
}

export = nocturne;

declare global {
    /** What the js runtime adds to `import.meta` of an ES module. */
    interface ImportMeta {
        /** The complete path of this module's document. */
        readonly filename: string;
        /** The complete path of the folder that holds this module. */
        readonly dirname: string;
        /** True when this module is the script that was started, false when it was imported. */
        readonly main: boolean;
        /** Resolves a module specifier the way an import does, and returns the result. */
        resolve(specifier: string): string;
    }
}
