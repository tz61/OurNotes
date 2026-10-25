// Saving to OurNotes, which must be running: OurNotes listens on 127.0.0.1:23120 for this
// extension (Preferences → Advanced). The popup asks where things may go; what it chose is
// saved by background.js (see there), with these.
'use strict';

const OURNOTES = 'http://127.0.0.1:23120/connector';

async function ourNotesCall(path, {json, body, contentType} = {}) {
  const init = {method: json || body ? 'POST' : 'GET', headers: {'X-OurNotes-Connector': '1'}};
  if (json) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(json);
  } else if (body) {
    init.headers['Content-Type'] = contentType;
    init.body = body;
  }
  const res = await fetch(OURNOTES + path, init);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `OurNotes answered ${res.status}`);
  return data;
}

// The PDF as this browser gets it (with the site's sign-in), else fetched by OurNotes. Under
// an item (parent, library), or on its own (into target, with tags).
async function ourNotesSavePdf(url, {parent = '', library = 0, title = '', target = '', tags = []} = {}) {
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
    const q = new URLSearchParams({parent, library: library || '', url, title, target, tags: JSON.stringify(tags)});
    return ourNotesCall(`/attachment?${q}`, {body: bytes, contentType: 'application/pdf'});
  }
  return ourNotesCall('/attachmentUrl', {json: {parent, library: library || 0, url, title, target, tags}});
}

// What the popup chose: the page's item and its PDF (or a PDF on its own) into target, with
// tags. progress(text) says how far it is; the result's pdf says how the PDF went.
async function ourNotesSave({page, title, target, tags}, progress = () => {}) {
  if (page.isPdf) {
    progress('Saving the PDF…');
    const result = await ourNotesSavePdf(page.pdfUrl, {title, target, tags});
    return {...result, pdf: 'OurNotes is looking up its details.'};
  }
  const result = await ourNotesCall('/save', {json: {item: page.item, target, tags}});
  if (!page.pdfUrl) return result;
  progress('Saving the PDF…');
  try {
    await ourNotesSavePdf(page.pdfUrl, {parent: result.key, library: result.libraryId, title: page.item.title});
    return {...result, pdf: 'PDF saved.'};
  } catch (e) {
    return {...result, pdf: `No PDF: ${e.message}`, pdfFailed: true};
  }
}
