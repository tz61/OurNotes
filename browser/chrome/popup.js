// Saves the page in the current tab to OurNotes, which must be running: OurNotes
// listens on 127.0.0.1:23120 for this extension (Preferences → Advanced).
'use strict';

const BASE = 'http://127.0.0.1:23120/connector';
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

async function call(path, {json, body, contentType} = {}) {
  const init = {method: json || body ? 'POST' : 'GET', headers: {'X-OurNotes-Connector': '1'}};
  if (json) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(json);
  } else if (body) {
    init.headers['Content-Type'] = contentType;
    init.body = body;
  }
  const res = await fetch(BASE + path, init);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `OurNotes answered ${res.status}`);
  return data;
}

function problem(text) {
  $('status').textContent = 'Not saved';
  $('saved').hidden = true;
  $('problem').hidden = false;
  $('problemText').textContent = text;
}

let saved = null;  // {key, libraryId, ...} of what was saved

function showSaved(result, title, itemType) {
  saved = result;
  $('status').textContent = 'Saved to OurNotes';
  $('problem').hidden = true;
  $('saved').hidden = false;
  $('title').textContent = result.title || title;
  $('kind').textContent = typeLabels[itemType || result.itemType] || '';
  const select = $('target');
  select.textContent = '';
  for (const t of result.targets || []) {
    const option = document.createElement('option');
    option.value = t.id;
    option.textContent = ' '.repeat(t.depth) + t.name;  // collections indented under their library
    select.append(option);
  }
  select.value = result.target;
}

function pdfStatus(text, failed) {
  $('pdf').hidden = false;
  $('pdf').textContent = text;
  $('pdf').classList.toggle('failed', !!failed);
}

// The PDF as this browser gets it (with the site's sign-in), else fetched by OurNotes.
async function savePdf(url, parent, libraryId, title) {
  let bytes = null;
  try {
    const res = await fetch(url, {credentials: 'include'});
    if (res.ok) {
      const buf = await res.arrayBuffer();
      const head = new Uint8Array(buf.slice(0, 5));
      if (String.fromCharCode(...head) === '%PDF-') bytes = buf;
    }
  } catch (e) {
    // another site, without this extension's access: OurNotes tries
  }
  if (bytes) {
    const q = new URLSearchParams({parent, library: libraryId || '', url, title});
    return call(`/attachment?${q}`, {body: bytes, contentType: 'application/pdf'});
  }
  return call('/attachmentUrl', {json: {parent, library: libraryId || 0, url, title}});
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
  if (/\.pdf([?#]|$)/i.test(tab.url)) return {item: null, pdfUrl: tab.url, isPdf: true, title: tab.title};
  return {item: {itemType: 'webpage', title: tab.title, url: tab.url}, pdfUrl: '', isPdf: false};
}

async function save() {
  $('status').textContent = 'Saving to OurNotes…';
  $('problem').hidden = true;
  const [tab] = await chrome.tabs.query({active: true, currentWindow: true});
  if (!tab || !/^(https?|file):/.test(tab.url || '')) {
    problem('Only web pages and PDFs can be saved.');
    return;
  }
  try {
    await call('/ping');
  } catch (e) {
    problem('OurNotes is not running, or its browser connector is off (Preferences → Advanced). Start it and try again.');
    return;
  }
  const page = await readPage(tab);
  try {
    if (page.isPdf) {
      pdfStatus('Saving the PDF…');
      const result = await savePdf(page.pdfUrl, '', 0, page.title || tab.title);
      showSaved(result, page.title || tab.title, 'attachment');
      pdfStatus('OurNotes is looking up its details.');
      return;
    }
    const result = await call('/save', {json: {item: page.item}});
    showSaved(result, page.item.title, page.item.itemType);
    $('tags').value = (page.item.tags || []).join(', ');
    if (page.pdfUrl) {
      pdfStatus('Saving the PDF…');
      try {
        await savePdf(page.pdfUrl, result.key, result.libraryId, page.item.title);
        pdfStatus('PDF saved.');
      } catch (e) {
        pdfStatus(`No PDF: ${e.message}`, true);
      }
    }
  } catch (e) {
    problem(e.message);
  }
}

async function update() {
  if (!saved) return;
  const tags = $('tags').value.split(',').map((t) => t.trim()).filter(Boolean);
  try {
    await call('/update', {json: {key: saved.key, library: saved.libraryId, target: $('target').value, tags}});
    saved.target = $('target').value;
  } catch (e) {
    pdfStatus(e.message, true);
  }
}

$('target').addEventListener('change', update);
$('tags').addEventListener('change', update);
$('tags').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') update().then(() => window.close());
});
$('retry').addEventListener('click', save);
$('done').addEventListener('click', () => update().then(() => window.close()));
save();
