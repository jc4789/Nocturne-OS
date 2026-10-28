# The execCommand draft's implementation

`implementation.js` is the implementation of the W3C Editing Task Force's
execCommand draft (https://w3c.github.io/editing/docs/execCommand/), written
by its editor alongside the draft and kept in web-platform-tests as
`editing/include/implementation.js`.

- Origin: https://github.com/web-platform-tests/wpt, tree
  `cd1d8aa5bfebc15d1af2af230c7ff95f9e582a4c`; the file was last changed
  upstream in `7c62c38c357771c69a3a09124d1b076e35b55f0c` (2016-08-24).
- License: 3-Clause BSD, `LICENSE.md` (WPT's, unchanged).
- Hashes: `upstream.json` (the upstream file, and the file as it is here).
  `user/libc/web/js_embed.py` refuses to generate `js_bootstrap.inc` when this
  file's hash is not the one recorded there, so that every change to it is
  listed below and in the manifest.

## Where it runs

`user/libc/web/js_editing.js` wraps it in one function, which `js_embed.py`
embeds as the `js_editing` program of `js_bootstrap.inc`.  A realm runs that
program (`js.c`, `native_editing`) the first time a page asks about or runs a
command, or the user edits an editing host; `js_document_commands.js` owns the
Document methods, `designMode`, `contentEditable` and `isContentEditable` and
calls it.  The names the file reads as globals (`document`, `Node`,
`getSelection`, `getComputedStyle`) are bound there to the realm's own objects,
with `cssColor`, `legacyFontPixels`, `fontSizePixels`, `editingHost` and
`editable`, which the changes below use.

## Local changes

Each is marked `Nocturne:` in the file.

1. `commands` has no prototype, so that "toString" or "constructor" is not a
   supported command.
2. `normalizeColor` asks the browser's CSS color parser (`cssColor`) instead of
   styling spans appended to the body and reading their computed color back
   (page-visible mutations, and no answer without a body).  The three color
   commands share one `getColorCommandValue` for their first steps.
3. `isEditingHost` and `isEditable` are HTML's definitions, owned by the
   bindings, reading the contenteditable attribute's state (Plaintext-Only
   included) and the document's design mode, not the page-replaceable
   accessors.
4. `isCollapsedLineBreak` and `isExtraneousLineBreak` answer from the tree and
   the computed display and white-space (`getLineBlockOf`,
   `getLineContribution`, `hasLineContentBeside`) instead of upstream's
   measurement, which inserted a zero-width space or hid the br, compared
   `offsetHeight` and restored the style attributes: that mutated the document
   on every query (reaching the page's MutationObservers and the undo history)
   and threw when the block had no height.
5. `getActiveRange` returns a range whose boundary points are in a comment, a
   processing instruction or the Document itself (designMode puts them there)
   instead of throwing; no such node is editable, so only the miscellaneous
   commands are enabled there.
6. `isBefore` takes null as the end of the tree, the stop of the walks over a
   range that ends in the document's last node; upstream passed null to
   `compareDocumentPosition`, which throws.
7. `areLooselyEquivalentValues` and `getLegacyFontSize` take the pixel sizes of the
   font keywords and of `<font size>` from the CSS engine (`legacyFontPixels`,
   `fontSizePixels`) instead of appending font elements to the body.
8. `insertText` takes `value` a code point at a time, and the `delete` and
   `forwardDelete` actions take a whole surrogate pair; `forwardDelete`'s
   combining marks are the Unicode general category M.

## Where Nocturne follows another standard than the draft

- The draft's `execCommand` step 4 fires `beforeinput` at the affected editing
  host.  Input Events says a user agent "MUST NOT dispatch [beforeinput] due to
  events that are not caused by attempted user input", and WPT's
  `input-events/input-events-exec-command.html` and
  `editing/other/exec-command-with-text-editor.tentative.html` test that a
  script's command fires none; `js_editing.js` fires none, and fires
  `beforeinput` for the user's own edits.
- The draft's table maps a command to an inputType for some commands only;
  for italic, underline, subscript, hiliteColor and removeFormat,
  `js_editing.js` uses the Input Events inputType of the same intent, and the
  input event's `data` is what Input Events defines for that inputType (the
  text, the URL, the font family, the serialized color) where the draft says
  null.
- The draft does not define undo and redo; `js_editing.js` keeps a history of
  the mutation records of each command and of the user's edits.
- copy, cut and paste, which the draft lets a user agent leave unsupported,
  are unsupported: they need the Clipboard API's events (ClipboardEvent,
  DataTransfer) and a clipboard reachable with the user's activation.
