/* Native-value regression cases, intended for web_live/QuickJS. No picker
   feature is counted merely because a type keyword is recognized. */
function runFormControlCases() {
    let count = 0;
    const equal = (actual, expected, label) => {
        count++;
        if (!Object.is(actual, expected)) throw new Error((label || 'input') + ': ' + String(actual) + ' != ' + String(expected));
    };
    const throws = (name, fn) => {
        count++; try { fn(); } catch (error) { if (error.name === name) return; throw error; }
        throw new Error('Expected ' + name);
    };
    const input = (type, value) => {
        const node = document.createElement('input');
        if (type !== undefined) node.type = type;
        if (value !== undefined) node.value = value;
        return node;
    };
    const supported = ['text','search','tel','url','email','password','hidden','date','month','week','time','datetime-local',
        'number','range','color','checkbox','radio','file','submit','image','reset','button'];
    equal(input().type, 'text');
    for (const type of supported) {
        const node = input(type.toUpperCase());
        equal(node.type, type, 'type ' + type);
        equal(node.getAttribute('type'), type.toUpperCase());
        equal(node instanceof HTMLInputElement, true);
    }
    for (const unknown of ['', 'bogus', 'datetime', 'NUMBER ', ' date']) equal(input(unknown).type, 'text');
    throws('TypeError', () => { input().type = Symbol(); });
    for (const type of ['text','search','tel','password','unknown']) {
        const node = input(type, ' a\rb\nc\r\n ');
        equal(node.value, ' abc ');
        node.value = null; equal(node.value, '');
        node.value = undefined; equal(node.value, 'undefined');
        node.value = '\t spaced \f'; equal(node.value, '\t spaced \f');
    }
    for (const type of ['url', 'email']) {
        equal(input(type, '\t https:\r\n//example.test/ \f').value, 'https://example.test/');
        equal(input(type, '  a b  ').value, 'a b');
        equal(input(type, '\vvalue\v').value, '\vvalue\v');
    }
    const email = input('email', ' a@b.test ,  c@d.test  ');
    equal(email.value, 'a@b.test ,  c@d.test');
    email.multiple = true; equal(email.value, 'a@b.test,c@d.test');
    email.value = ' , a@b.test , , c@d.test, '; equal(email.value, ',a@b.test,,c@d.test,');
    email.multiple = false; equal(email.value, ',a@b.test,,c@d.test,');

    for (const type of ['hidden','submit','reset','image','button']) {
        const node = input(type); equal(node.value, '');
        node.value = 'a\nb'; equal(node.value, 'a\nb'); equal(node.getAttribute('value'), 'a\nb');
        node.defaultValue = 'default'; equal(node.value, 'default');
    }
    for (const type of ['checkbox','radio']) {
        const node = input(type); equal(node.value, 'on');
        node.value = ''; equal(node.value, ''); equal(node.getAttribute('value'), '');
        node.removeAttribute('value'); equal(node.value, 'on');
    }
    const file = input('file'); equal(file.value, '');
    file.value = ''; equal(file.value, '');
    throws('InvalidStateError', () => { file.value = 'local.txt'; });
    file.defaultValue = 'local.txt'; equal(file.value, '');

    const number = input('number'); equal(Number.isNaN(number.valueAsNumber), true);
    for (const [raw, expected] of [['0',0],['-0',0],['1',1],['001',1],['-2.5',-2.5],['.5',0.5],['-.5',-0.5],
        ['1e3',1000],['1E-2',0.01],['1e+2',100],['1e-400',0]]) {
        number.value = raw; equal(number.value, raw); equal(number.valueAsNumber, expected, raw);
    }
    for (const raw of ['', '+1',' 1','1 ','1.','.', '-', '1e', '1e+', '0x10','1,5','1a','NaN','Infinity','-Infinity','1e400','1\n2']) {
        number.value = raw; equal(number.value, ''); equal(Number.isNaN(number.valueAsNumber), true, raw);
    }
    for (const value of [0,-0,0.1,0.01,0.000001,1e-7,1e20,1e21,1.2345678901234567,-2.5,Number.MAX_VALUE,Number.MIN_VALUE]) {
        number.valueAsNumber = value;
        equal(number.value, String(value), 'number serialization');
        equal(number.valueAsNumber, value === 0 ? 0 : value);
    }
    number.valueAsNumber = NaN; equal(number.value, '');
    throws('TypeError', () => { number.valueAsNumber = Infinity; });
    throws('TypeError', () => { number.valueAsNumber = -Infinity; });
    throws('TypeError', () => { number.valueAsNumber = 1n; });
    throws('TypeError', () => { number.valueAsNumber = {valueOf(){return 1n;}}; });
    throws('TypeError', () => { number.valueAsNumber = Symbol(); });
    number.valueAsNumber = null; equal(number.value, '0');
    const text = input('text', 'abc'); equal(Number.isNaN(text.valueAsNumber), true); equal(text.valueAsDate, null);
    throws('InvalidStateError', () => { text.valueAsNumber = 5; });
    throws('TypeError', () => { text.valueAsNumber = Infinity; });
    throws('InvalidStateError', () => { text.valueAsDate = new Date(); });

    const valid = {
        date: ['0001-01-01','1900-02-28','2000-02-29','2024-02-29','1970-01-01','2026-12-31'],
        month: ['0001-01','1970-01','1969-12','2026-10','10000-12'],
        week: ['1970-W01','2015-W53','2020-W53','2021-W01','2026-W53'],
        time: ['00:00','23:59','12:34:56','12:34:56.1','12:34:56.12','12:34:56.123','00:00:00.000'],
        'datetime-local': ['1970-01-01T00:00','2024-02-29T23:59:59.123','2026-10-07 12:34:00.120']
    };
    const invalid = {
        date: ['0000-01-01','2023-02-29','1900-02-29','2026-04-31','2026-00-01','2026-13-01','2026-1-01','26-01-01','2026-01-01Z'],
        month: ['0000-01','2026-00','2026-13','2026-1','2026-01-01'],
        week: ['2021-W53','2014-W53','2026-W00','2026-W54','2026-W1','2026-w01','26-W01'],
        time: ['24:00','12:60','12:34:60','1:00','12:3','12:34.5','12:34:56.','12:34:56.1234','12:34Z',' 12:34'],
        'datetime-local': ['2023-02-29T12:34','2026-10-07T24:00','2026-10-07t12:34','2026-10-07T12:34Z','2026-10-07T12:34+01:00','2026-10-07']
    };
    for (const type of Object.keys(valid)) {
        const node = input(type);
        for (const raw of valid[type]) {
            node.value = raw;
            const expected = type === 'datetime-local' ? raw.replace(' ', 'T').replace(':00.120', ':00.12') : raw;
            equal(node.value, expected, type + ' ' + raw); equal(Number.isFinite(node.valueAsNumber), true);
        }
        for (const raw of invalid[type]) {
            node.value = raw; equal(node.value, '', type + ' ' + raw); equal(Number.isNaN(node.valueAsNumber), true);
        }
        node.value = ''; equal(node.valueAsDate, null);
    }
    const date = input('date', '1970-01-01'); equal(date.valueAsNumber, 0); equal(date.valueAsDate.getTime(), 0);
    date.valueAsNumber = -86400000; equal(date.value, '1969-12-31');
    date.valueAsDate = new Date('2026-10-07T23:59:59.999Z'); equal(date.value, '2026-10-07');
    date.valueAsDate = new Date(NaN); equal(date.value, '');
    date.valueAsDate = null; equal(date.value, '');
    throws('TypeError', () => { date.valueAsDate = 0; });
    throws('TypeError', () => { date.valueAsDate = {getTime(){return 0;}}; });
    throws('TypeError', () => { date.valueAsDate = undefined; });
    const month = input('month', '1969-12'); equal(month.valueAsNumber, -1); equal(month.valueAsDate.getTime(), Date.UTC(1969,11,1));
    month.valueAsNumber = 0; equal(month.value, '1970-01');
    month.valueAsDate = new Date('2026-10-07T10:11:12Z'); equal(month.value, '2026-10');
    const week = input('week', '1970-W01'); equal(week.valueAsNumber, -259200000); equal(week.valueAsDate.getTime(), -259200000);
    week.valueAsDate = new Date('2016-01-01T12:00:00Z'); equal(week.value, '2015-W53');
    week.valueAsNumber = 0; equal(week.value, '1970-W01');
    const time = input('time', '12:34:56.123'); equal(time.valueAsNumber, 45296123); equal(time.valueAsDate.getTime(), 45296123);
    time.valueAsNumber = 0; equal(time.value, '00:00');
    time.valueAsDate = new Date('2026-10-07T12:34:56.120Z'); equal(time.value, '12:34:56.12');
    const local = input('datetime-local', '1970-01-01 00:00:00.000'); equal(local.value, '1970-01-01T00:00'); equal(local.valueAsNumber, 0);
    equal(local.valueAsDate, null); throws('InvalidStateError', () => { local.valueAsDate = new Date(); });
    local.valueAsNumber = 86400000 + 45296123; equal(local.value, '1970-01-02T12:34:56.123');

    const range = input('range'); equal(range.value, '50'); equal(range.valueAsNumber, 50);
    range.min = '10'; range.max = '20'; equal(range.value, '20');
    range.value = ''; equal(range.value, '15');
    range.value = '0'; equal(range.value, '10'); range.value = '40'; equal(range.value, '20');
    range.step = '3'; range.value = '15'; equal(range.value, '16');
    range.value = '20'; equal(range.value, '19');
    range.step = 'any'; range.value = '15.5'; equal(range.value, '15.5');
    throws('InvalidStateError', () => range.stepUp());
    range.min = '25'; range.value = ''; equal(range.value, '25');
    const tie = input('range'); tie.min = '0'; tie.max = '100'; tie.step = '20'; tie.value = '50'; equal(tie.value, '60');
    tie.step = 'any'; tie.value = '0002'; equal(tie.value, '0002');
    const color = input('color'); equal(color.value, '#000000');
    color.value = '#A1B2C3'; equal(color.value, '#a1b2c3'); color.value = 'bad'; equal(color.value, '#000000');

    for (const [type, value, delta] of [['date','2024-02-28',86400000],['month','2026-01',1],['week','2020-W53',604800000],
        ['time','12:00',60000],['datetime-local','2026-10-07T12:00',60000],['number','1',1],['range','50',1]]) {
        const node = input(type, value); const before = node.valueAsNumber;
        node.stepUp(); equal(node.valueAsNumber, before + delta, type + ' stepUp');
        node.stepDown(2); equal(node.valueAsNumber, before - delta, type + ' stepDown');
        for (const step of ['0','-2','bogus']) {
            node.step = step; node.value = value; node.stepUp(); equal(node.valueAsNumber, before + delta);
        }
        node.step = '2'; node.value = value; node.stepUp();
        /* Explicit steps align against zero/default week base before stepping. */
        const aligned = node.valueAsNumber, scale = ['time','datetime-local'].includes(type) ? 1000 : delta;
        node.stepUp(); equal(node.valueAsNumber, aligned + scale * 2);
    }
    for (const type of supported.filter(type => !['date','month','week','time','datetime-local','number','range'].includes(type))) {
        const node = input(type); throws('InvalidStateError', () => node.stepUp()); throws('InvalidStateError', () => node.stepDown());
    }
    const step = input('number', '0'); step.step = '0.1'; step.stepUp(3); equal(step.valueAsNumber, 0.3);
    step.value = '0.3'; step.stepUp(); equal(step.valueAsNumber, 0.4);
    step.step = '3'; step.value = '2'; step.stepUp(5); equal(step.value, '3');
    step.value = '2'; step.stepDown(5); equal(step.value, '0');
    step.min = '0'; step.max = '7'; step.value = '0'; step.stepUp(3); equal(step.value, '6');
    step.min = '10'; step.max = '5'; step.value = '0'; step.stepUp(); equal(step.value, '0');
    step.min = ''; step.max = '0'; step.value = '1'; step.stepUp(); equal(step.value, '1');
    step.min = '10'; step.max = ''; step.value = '1'; step.stepDown(); equal(step.value, '1');
    step.min = ''; step.step = '1'; step.value = '0'; step.stepUp(4294967297); equal(step.value, '1');
    throws('TypeError', () => step.stepUp(1n)); throws('TypeError', () => step.stepDown(Symbol()));

    const mutable = input('text', '42'); mutable.type = 'number'; equal(mutable.value, '42');
    mutable.value = 'not a number'; equal(mutable.value, ''); mutable.type = 'text'; equal(mutable.value, '');
    mutable.value = 'current'; mutable.type = 'hidden'; equal(mutable.value, 'current'); equal(mutable.defaultValue, 'current');
    mutable.value = 'new default'; mutable.type = 'text'; equal(mutable.value, 'new default');
    mutable.defaultValue = 'follow'; equal(mutable.value, 'follow'); mutable.value = 'dirty'; mutable.defaultValue = 'ignored'; equal(mutable.value, 'dirty');
    mutable.type = 'file'; equal(mutable.value, ''); mutable.type = 'text'; equal(mutable.value, 'ignored');
    const form = document.createElement('form'), reset = input('number'); form.append(reset); document.body.append(form);
    reset.defaultValue = 'bad'; equal(reset.value, ''); reset.value = '10'; form.reset(); equal(reset.value, '');
    reset.defaultValue = '12'; equal(reset.value, '12'); reset.value = '13'; form.reset(); equal(reset.value, '12');
    form.remove();
    const select = document.createElement('select'); select.multiple = true;
    const oa = new Option('Alpha', 'a', true, true), ob = new Option('Beta', 'b', false, true), oc = new Option('Gamma', 'c');
    const choices = select.options, chosen = select.selectedOptions;
    equal(choices instanceof HTMLOptionsCollection, true); equal(choices instanceof HTMLCollection, true);
    equal(Array.isArray(choices), false); equal(Object.getPrototypeOf(choices), HTMLOptionsCollection.prototype);
    equal(Object.prototype.toString.call(choices), '[object HTMLOptionsCollection]');
    equal(chosen instanceof HTMLCollection, true); equal(chosen instanceof HTMLOptionsCollection, false);
    equal(select.options, choices); equal(select.selectedOptions, chosen); equal(choices.length, 0);
    select.add(oa); choices.add(ob); select.appendChild(oc);
    equal(select.length, 3); equal(choices.length, 3); equal(choices.item(0), oa); equal(choices[1], ob);
    equal(chosen.length, 2); equal(chosen[0], oa); equal(chosen[1], ob); equal(select.value, 'a'); equal(select.selectedIndex, 0);
    equal(oa.defaultSelected, true); equal(ob.defaultSelected, false);
    oa.selected = false; equal(oa.defaultSelected, true); equal(oa.getAttribute('selected'), '');
    equal(chosen.length, 1); equal(chosen[0], ob); equal(select.value, 'b'); equal(select.selectedIndex, 1);
    oa.defaultSelected = false; oa.defaultSelected = true; equal(oa.selected, false, 'dirty selectedness ignores default');
    oc.defaultSelected = true; equal(oc.selected, true, 'clean selectedness follows default');
    equal(chosen.length, 2); equal(oc.index, 2); equal(new Option().index, 0);
    select.value = 'c'; equal(chosen.length, 1); equal(chosen[0], oc); equal(oc.defaultSelected, true);
    select.value = 'missing'; equal(chosen.length, 0); equal(select.selectedIndex, -1); equal(select.value, '');
    choices.selectedIndex = 1; equal(chosen.length, 1); equal(chosen[0], ob); equal(choices.selectedIndex, 1);
    select.selectedIndex = -1; equal(chosen.length, 0); select.selectedIndex = 99; equal(select.selectedIndex, -1);
    oa.selected = true; ob.selected = true; oc.selected = true; equal(chosen.length, 3);
    select.multiple = false; equal(chosen.length, 1); equal(chosen[0], oa, 'multiple removal keeps first selected');
    ob.selected = true; equal(oa.selected, false); equal(oc.selected, false); equal(select.selectedIndex, 1);
    ob.selected = false; equal(select.selectedIndex, 0, 'single size1 restores first enabled');
    select.size = 3; select.selectedIndex = -1; equal(select.selectedIndex, -1);
    select.size = 1; equal(select.selectedIndex, 0); select.selectedIndex = -1; equal(select.selectedIndex, -1);
    select.size = 0; equal(select.selectedIndex, -1, 'literal display size zero is not one');
    const listbox = document.createElement('select'); listbox.size = 3; listbox.add(new Option('one')); listbox.add(new Option('two'));
    equal(listbox.selectedIndex, -1); equal(listbox.selectedOptions.length, 0);
    const selectedFirst = new Option('inserted', 'first', false, true); select.size = 1;
    select.insertBefore(selectedFirst, select.firstChild); equal(select.value, 'first'); equal(select.selectedIndex, 0);
    select.insertBefore(oa, selectedFirst); equal(select.value, 'first', 'unselected move preserves current selectedness');
    equal(selectedFirst.index, 1); select.add(selectedFirst, selectedFirst); equal(selectedFirst.index, 1);
    throws('NotFoundError', () => select.add(new Option(), document.createElement('option')));
    throws('TypeError', () => select.add(document.createElement('div')));
    throws('TypeError', () => choices.add()); throws('TypeError', () => choices.remove());
    throws('TypeError', () => select.selectedIndex = 1n); throws('TypeError', () => select.length = Symbol());
    oa.id = 'named-option'; oa.setAttribute('name', 'choice');
    equal(choices.namedItem('named-option'), oa); equal(choices.choice, oa); equal(select.namedItem('choice'), oa); equal(select.item(0), oa);
    equal(Object.keys(choices).includes('choice'), false); equal(Object.keys(choices).includes('0'), true);
    equal([...choices].length, select.length); equal(HTMLCollection.prototype.item.call(choices, 1), selectedFirst);
    throws('TypeError', () => HTMLOptionsCollection.prototype.add.call({}, oa));
    throws('TypeError', () => new HTMLOptionsCollection());
    const defaultsBefore = [oa.defaultSelected, ob.defaultSelected, oc.defaultSelected];
    choices.length = 6; equal(select.length, 6); equal(choices[5].text, ''); equal(choices[5].parentNode, select);
    const replacement = new Option('replacement', 'r'); choices[1] = replacement;
    equal(choices[1], replacement); equal(selectedFirst.parentNode, null); equal(replacement.index, 1);
    equal(oa.defaultSelected, defaultsBefore[0]); equal(ob.defaultSelected, defaultsBefore[1]); equal(oc.defaultSelected, defaultsBefore[2]);
    choices[1] = null; equal(replacement.parentNode, null); equal(choices.length, 5);
    choices[7] = new Option('gap', 'gap'); equal(choices.length, 8); equal(choices[6].text, ''); equal(choices[7].value, 'gap');
    choices[4294967294] = null; equal(choices.length, 8);
    choices.length = 100001; equal(choices.length, 8, 'oversized length expansion ignored');
    choices.length = 3; equal(choices.length, 3); equal(select.length, 3);
    choices.remove(-1); equal(choices.length, 3); select.remove(2); equal(choices.length, 2);
    const group = document.createElement('optgroup'); group.append(new Option('group', 'g'));
    select.add(group, 0); equal(choices[0].value, 'g'); equal(choices[0].parentNode, group);
    choices.length = 0; equal(group.parentNode, select); equal(group.children.length, 0); equal(chosen.length, 0);
    const textOption = new Option(); textOption.textContent = ' \t alpha\n beta \r '; textOption.label = 'label only';
    equal(textOption.text, 'alpha beta'); equal(textOption.value, 'alpha beta', 'value fallback does not use label'); equal(textOption.label, 'label only');
    const script = document.createElement('script'); script.textContent = 'not option text'; textOption.append(script); equal(textOption.text, 'alpha beta');
    textOption.text = 'new text'; equal(textOption.children.length, 0); equal(textOption.value, 'new text');
    const defaultOption = new Option('default', undefined, true); equal(defaultOption.defaultSelected, true); equal(defaultOption.selected, false);
    defaultOption.defaultSelected = false; defaultOption.defaultSelected = true; equal(defaultOption.selected, true, 'factory selectedness is initially clean');
    const selectForm = document.createElement('form'), resetSelect = document.createElement('select'); resetSelect.multiple = true;
    const rd = new Option('d', 'd', true, true), re = new Option('e', 'e'); resetSelect.append(rd, re); selectForm.append(resetSelect); document.body.append(selectForm);
    rd.selected = false; re.selected = true; selectForm.reset(); equal(rd.selected, true); equal(re.selected, false); equal(resetSelect.selectedOptions.length, 1);
    re.defaultSelected = true; equal(re.selected, true, 'reset clears option dirtiness');
    const clone = resetSelect.cloneNode(true); equal(clone.options.length, 2); equal(clone.selectedOptions.length, 2);
    clone.options[0].selected = false; equal(rd.selected, true); equal(clone.options[0].defaultSelected, true);
    selectForm.remove(); document.body.append(select); select.remove(); equal(select.parentNode, null); equal(choices.length, 0);
    const emptySelect = document.createElement('select'), emptyA = new Option('a', 'a'), emptyB = new Option('b', 'b');
    emptySelect.append(emptyA, emptyB); emptySelect.selectedIndex = -1;
    const emptyChosen = emptySelect.selectedOptions;
    emptyA.text = 'updated'; equal(emptySelect.selectedIndex, -1, 'option text does not reset empty selection');
    const harmless = document.createTextNode('text'); emptySelect.append(harmless);
    equal(emptySelect.selectedIndex, -1, 'non-option insert preserves empty selection'); harmless.remove();
    equal(emptySelect.selectedIndex, -1, 'non-option removal preserves empty selection');
    emptySelect.appendChild(emptyA); equal(emptySelect.selectedIndex, -1, 'same-select option move preserves empty selection');
    equal(emptySelect.options[1], emptyA); equal(emptyChosen.length, 0);
    document.body.appendChild(emptySelect); equal(emptySelect.selectedIndex, -1, 'connecting select preserves empty selection');
    const emptyContainer = document.createElement('form'); emptyContainer.appendChild(emptySelect); document.body.appendChild(emptyContainer);
    equal(emptySelect.selectedIndex, -1, 'connecting enclosing container preserves empty selection');
    document.body.prepend(emptyContainer); equal(emptySelect.selectedIndex, -1, 'moving enclosing container preserves empty selection');
    const otherDocument = document.implementation.createHTMLDocument('select adoption');
    otherDocument.adoptNode(emptySelect); equal(emptySelect.selectedIndex, -1, 'adopting whole select preserves empty selection');
    otherDocument.body.appendChild(emptySelect); equal(emptySelect.selectedIndex, -1, 'connecting adopted select preserves empty selection');
    document.adoptNode(emptySelect); document.body.appendChild(emptySelect);
    equal(emptySelect.selectedIndex, -1); equal(emptySelect.value, ''); equal(emptyChosen.length, 0);
    emptySelect.remove(); emptyContainer.remove();
    const groupSelect = document.createElement('select');
    groupSelect.innerHTML = '<optgroup><option selected value="first">first</option><option selected value="last">last</option></optgroup>';
    equal(groupSelect.value, 'last', 'bulk option insertion preserves the last incoming selected option');
    equal(groupSelect.selectedOptions.length, 1);
    for (const property of ['options','selectedOptions','length','selectedIndex','value']) {
        const descriptor = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, property);
        throws('TypeError', () => descriptor.get.call({}));
        if (descriptor.set) throws('TypeError', () => descriptor.set.call({}, 1));
    }
    for (const property of ['selected','value','text','label','index']) {
        const descriptor = Object.getOwnPropertyDescriptor(HTMLOptionElement.prototype, property);
        throws('TypeError', () => descriptor.get.call({}));
        if (descriptor.set) throws('TypeError', () => descriptor.set.call({}, 'x'));
    }
    for (const property of ['type','value','valueAsNumber','valueAsDate','min','max','step']) {
        const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, property);
        throws('TypeError', () => descriptor.get.call({}));
        throws('TypeError', () => descriptor.set.call({}, property === 'valueAsDate' ? null : '1'));
    }
    throws('TypeError', () => HTMLInputElement.prototype.stepUp.call({}));
    return count;
}
