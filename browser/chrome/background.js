// Saves what the popup chose when its Done is clicked. Here rather than in the popup, which
// closes as soon as the browser is clicked elsewhere: what was chosen is then saved all the
// same (its PDF too), and a failure shows on the button.
'use strict';

importScripts('save.js');

chrome.runtime.onMessage.addListener((message, sender, reply) => {
  if (!message || message.type !== 'save') return false;
  chrome.action.setBadgeText({text: ''});
  chrome.action.setTitle({title: 'Save to OurNotes'});
  const progress = (text) => chrome.runtime.sendMessage({type: 'progress', text}).catch(() => {});
  ourNotesSave(message.job, progress).then(
    (result) => reply({ok: true, result}),
    (e) => {
      chrome.action.setBadgeBackgroundColor({color: '#c0392b'});
      chrome.action.setBadgeText({text: '!'});
      chrome.action.setTitle({title: `Not saved to OurNotes: ${e.message}`});
      reply({ok: false, error: e.message});
    });
  return true;  // (answered when saved)
});
