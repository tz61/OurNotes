// Shows what the page in the current tab would be saved as, and where to, in OurNotes.
// Nothing is saved until Done: Cancel, Esc or clicking elsewhere in the browser (which
// closes the popup) leave the library as it was. Done hands it to background.js, which
// saves it even if the popup closes meanwhile.
'use strict';

const $ = (id) => document.getElementById(id);

const typeLabels = {
  journalArticle: 'Journal Article',
  conferencePaper: 'Conference Paper',
  preprint: 'Preprint',
  bookSection: 'Book Section',
  book: 'Book',
  thesis: 'Thesis',
  report: 'Report',
  newspaperArticle: 'Newspaper Article',
  blogPost: 'Blog Post',
  webpage: 'Web Page',
  attachment: 'PDF',
};

let job = null;  // what Done saves: {page, title}
let saving = false;

function problem(text) {
  $('status').textContent = 'Not saved';
  $('form').hidden = true;
  $('problem').hidden = false;
  $('problemText').textContent = text;
  $('done').disabled = true;
}

function pdfStatus(text, failed) {
  $('pdf').hidden = false;
  $('pdf').textContent = text;
  $('pdf').classList.toggle('failed', !!failed);
}

async function readPage(tab) {
  try {
    await chrome.scripting.executeScript({target: {tabId: tab.id}, files: ['extract.js']});
    const [result] = await chrome.scripting.executeScript({
      target: {tabId: tab.id},
      func: () => ourNotesExtract(document, location.href),
    });
    if (result && result.result) return result.result;
  } catch (e) {
    // a page scripts may not run in: its address and title will do
  }
  if (/\.pdf([?#]|$)/i.test(tab.url) || ourNotesArxivAbs(tab.url))
    return {item: null, pdfUrl: tab.url, isPdf: true, title: tab.title};
  return {item: {itemType: 'webpage', title: tab.title, url: tab.url}, pdfUrl: '', isPdf: false};
}

// An arXiv PDF: what its abstract page says (as when saved from there), with this PDF.
async function arxivPaper(pdfUrl) {
  const abs = ourNotesArxivAbs(pdfUrl);
  if (!abs) return null;
  try {
    const res = await fetch(abs, {credentials: 'include'});
    if (!res.ok) return null;
    const page = ourNotesExtract(new DOMParser().parseFromString(await res.text(), 'text/html'), abs);
    return page.item && page.item.itemType === 'preprint' ? {...page, pdfUrl} : null;
  } catch (e) {
    return null;  // saved as a PDF then, and OurNotes looks up its details
  }
}

// The page read and where it may go asked of OurNotes: shown, to be saved with Done.
async function prepare() {
  job = null;
  $('status').textContent = 'Reading the page…';
  $('problem').hidden = true;
  $('done').disabled = true;
  $('target').disabled = $('tags').disabled = false;
  const [tab] = await chrome.tabs.query({active: true, currentWindow: true});
  if (!tab || !/^(https?|file):/.test(tab.url || '')) {
    problem('Only web pages and PDFs can be saved.');
    return;
  }
  try {
    await ourNotesCall('/ping');
  } catch (e) {
    problem('OurNotes is not running, or its browser connector is off (Preferences → Advanced). Start it and try again.');
    return;
  }
  let places;
  try {
    places = await ourNotesCall('/targets');
  } catch (e) {
    problem('This OurNotes is older than the extension: update OurNotes.');
    return;
  }
  let page = await readPage(tab);
  if (page.isPdf) page = (await arxivPaper(page.pdfUrl)) || page;
  const title = page.isPdf ? page.title || tab.title : page.item.title;
  job = {page, title};
  $('status').textContent = 'Save to OurNotes';
  $('form').hidden = false;
  $('title').textContent = title;
  $('kind').textContent = typeLabels[page.isPdf ? 'attachment' : page.item.itemType] || '';
  const select = $('target');
  select.textContent = '';
  for (const t of places.targets || []) {
    const option = document.createElement('option');
    option.value = t.id;
    option.textContent = ' '.repeat(t.depth) + t.name;  // collections indented under their library
    select.append(option);
  }
  select.value = places.target;
  $('tags').value = page.isPdf ? '' : (page.item.tags || []).join(', ');
  if (page.isPdf) pdfStatus('The PDF; OurNotes looks up its details.');
  else if (page.pdfUrl) pdfStatus('With its PDF.');
  else $('pdf').hidden = true;
  $('done').disabled = false;
  $('done').focus();
}

async function save() {
  if (!job || saving) return;
  saving = true;
  for (const id of ['done', 'target', 'tags']) $(id).disabled = true;
  $('status').textContent = 'Saving to OurNotes…';
  const tags = $('tags').value.split(',').map((t) => t.trim()).filter(Boolean);
  let answer;
  try {
    answer = await chrome.runtime.sendMessage({type: 'save', job: {...job, target: $('target').value, tags}});
  } catch (e) {
    answer = {ok: false, error: e.message};
  }
  saving = false;
  if (!answer || !answer.ok) {
    problem(answer ? answer.error : 'Not saved.');
    return;
  }
  $('status').textContent = 'Saved to OurNotes';
  if (answer.result.pdf) pdfStatus(answer.result.pdf, answer.result.pdfFailed);
  setTimeout(() => window.close(), answer.result.pdfFailed ? 3000 : 800);
}

chrome.runtime.onMessage.addListener((message) => {
  if (message && message.type === 'progress' && saving) pdfStatus(message.text);
});
$('done').addEventListener('click', save);
$('cancel').addEventListener('click', () => window.close());
$('tags').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') save();
});
$('retry').addEventListener('click', prepare);
prepare();
