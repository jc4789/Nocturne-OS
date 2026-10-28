/* The cases of tests/cssom_bindings_cases.c: CSSOM through js_stylesheets.js and the
   native rule tree. Each names the WPT page or specification section it reads. */
'use strict';
const style = fixture('style', 0), otherStyle = fixture('style', 1);
const sheet = style.sheet, rules = sheet.cssRules;
function thrown(f) { try { f(); return ''; } catch (e) { return e.name; } }

/* CSS Conditional 3, CSSMediaRule.matches and CSSSupportsRule.matches
   (css/css-conditional/at-media-matches-001.html, at-supports-matches.html) */
check(rules[6] instanceof CSSMediaRule && rules[6].matches === true, '@media all matches');
check(rules[7].matches === false, '@media not all never matches');
check(rules[8].matches === true && rules[9].matches === false, '@media (min-width) against the document width (800)');
check(rules[10].matches === true, '@media with an empty media list matches');
check(rules[11].matches === true && rules[12].matches === false, '@supports matches as its condition');

/* CSS Cascade 5, CSSLayerStatementRule.nameList: a FrozenArray, the same object while the names are */
const layer = rules[5];
check(layer.nameList === layer.nameList, 'nameList is the same array each time');
check(Object.isFrozen(layer.nameList) && layer.nameList.length === 2 && layer.nameList[1] === 'b.c', 'nameList is frozen and holds the names');

/* CSS Animations 1: a keyframe's !important declarations are invalid
   (css/css-animations/KeyframeEffect-getKeyframes.tentative.html) */
const keyframes = rules[13];
check(keyframes.cssRules[0].cssText === '100% { margin-bottom: 200px; }', 'an !important declaration leaves a keyframe: ' + keyframes.cssRules[0].cssText);
check(keyframes.cssRules[1].cssText === '0% { color: blue; }', 'a later normal declaration stands in place of an ignored !important one: ' + keyframes.cssRules[1].cssText);
check(keyframes.cssRules[0].style.getPropertyValue('margin-top') === '', 'an ignored declaration reads as none');
keyframes.cssRules[1].style.setProperty('color', 'green', 'important');
check(keyframes.cssRules[1].cssText === '0% { color: blue; }', 'setProperty with !important changes no keyframe');
keyframes.cssRules[1].style.setProperty('color', 'green');
check(keyframes.cssRules[1].cssText === '0% { color: green; }', 'setProperty without a priority sets a keyframe declaration');

/* CSS Nesting 1, nested selectorText through the selector serializer (css/css-nesting/parsing.html) */
const nested = rules[14].cssRules, expected = ['& > .bar', '& + .baz, &.qux', ':is(.bar, &.baz)', '&&', '& [title="a&b"]',
    '.x &', '& > .y', '& > section, & > article', '& .p, & .q', '&:hover'];
for (let i = 0; i < expected.length; i++)
    check(nested[i] && nested[i].selectorText === expected[i], 'nested selectorText ' + expected[i] + ': ' + (nested[i] && nested[i].selectorText));
check(rules[14].cssText.includes('\n  & > .y {'), 'a nested rule serializes with its selectorText');

/* CSSOM, CSSStyleSheet(options): CSSStyleSheetInit (css/cssom/CSSStyleSheet-constructable-baseURL.html) */
check(thrown(() => new CSSStyleSheet({ baseURL: 'https://test:test/' })) === 'NotAllowedError', 'a baseURL that is not a URL throws NotAllowedError');
const order = [];
new CSSStyleSheet({ get media() { order.push('media'); return ''; }, get disabled() { order.push('disabled'); return false; },
    get baseURL() { order.push('baseURL'); return null; } });
check(order.join() === 'baseURL,disabled,media', 'CSSStyleSheetInit members are read in order: ' + order.join());
new CSSStyleSheet({ baseURL: 'custom/path/' });
check(fixture('newestBase') === 'https://parent.test/custom/path/', 'a relative baseURL is a URL against the document base: ' + fixture('newestBase'));
new CSSStyleSheet();
check(fixture('newestBase') === 'https://parent.test/page', 'without a baseURL the document base is the sheet base');
const disabled = new CSSStyleSheet({ disabled: true, media: sheet.media });
check(disabled.disabled === true && disabled.media.mediaText === '', 'disabled and a MediaList media are taken');
check(thrown(() => new CSSStyleSheet(5)) === 'TypeError', 'CSSStyleSheetInit is a dictionary');

/* CSSOM, CSSImportRule.styleSheet (css/cssom/cssimportrule.html, cssimportrule-parent.html,
   cssimportrule-sheet-identity.html, CSSImportRule-media-identity.html) */
const dataImport = rules[0], child = dataImport.styleSheet;
check(child instanceof CSSStyleSheet, 'a loaded @import has a CSSStyleSheet');
check(dataImport.styleSheet === child, 'styleSheet is the same object each time');
check(child && child.ownerRule === dataImport && child.parentStyleSheet === sheet, 'its ownerRule is the rule, its parent the rule\'s sheet');
check(child && child.ownerNode === null && child.title === null, 'an imported sheet has no owner node and no title');
check(child && child.href === 'data:text/css,.child%7Bcolor:red%7D', 'its href is the rule\'s URL: ' + (child && child.href));
check(child && dataImport.media === child.media && child.media.mediaText === 'screen', 'CSSImportRule.media is styleSheet.media');
check(child && child.cssRules.length === 1 && child.cssRules[0].selectorText === '.child' && child.cssRules[0].parentStyleSheet === child,
    'its rules are the resource\'s, their parent the imported sheet');
check(sheet.ownerRule === null && sheet.parentStyleSheet === null, 'a sheet with no rule has no ownerRule and no parent');
const imported = rules[1].styleSheet;
check(imported && imported.cssRules.length === 2 && imported.cssRules[0] instanceof CSSImportRule, 'an imported sheet has its own @import rules');
const nestedSheet = imported && imported.cssRules[0].styleSheet;
check(nestedSheet && nestedSheet.parentStyleSheet === imported && nestedSheet.cssRules[0].selectorText === '.nested',
    'an import inside an imported sheet resolves against that sheet\'s URL');
check(rules[2].styleSheet === null, 'an @import whose resource has not loaded has none');
check(rules[4].styleSheet === null, 'an @import whose supports() is false has none');
const cross = rules[3].styleSheet;
check(cross instanceof CSSStyleSheet && thrown(() => cross.cssRules) === 'SecurityError', 'a cross-origin imported sheet hides its rules');
const otherImport = otherStyle.sheet.cssRules[0].styleSheet;
check(otherImport instanceof CSSStyleSheet && otherImport !== imported, 'two @import rules of one URL have two sheets');
fixture('restyled');
child.insertRule('.added {}', 1);
check(child.cssRules.length === 2 && fixture('restyled'), 'a change to an imported sheet restyles the document');
child.disabled = true;
check(child.disabled === true && fixture('restyled'), 'disabling an imported sheet restyles the document');
child.disabled = false;
sheet.deleteRule(0);
check(dataImport.parentStyleSheet === null && child.parentStyleSheet === null, 'a removed @import rule\'s sheet has no parent');
check(child.ownerRule === dataImport && dataImport.styleSheet === child, 'the removed rule keeps its sheet');
fixture('restyled');
child.insertRule('.later {}', 0);
check(child.cssRules.length === 3 && !fixture('restyled'), 'a removed rule\'s sheet reaches no cascade');
