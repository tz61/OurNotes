// What a page says about itself, as an OurNotes (Zotero-style) item: the citation_*
// tags of scholarly sites (Google Scholar's), embedded JSON-LD, Dublin Core, Open
// Graph, and at least the title and address. Run in the page; also loaded by the tests.
//
// ourNotesExtract(document, url) -> {item, pdfUrl, isPdf, title}
//   item:   {itemType, title, creators, date, url, DOI, ...}; null for a PDF tab
//   pdfUrl: the full text's PDF, when the page names one
(function (global) {
  'use strict';

  function clean(s) {
    return typeof s === 'string' ? s.replace(/\s+/g, ' ').trim() : '';
  }

  function metaTags(doc) {
    const metas = new Map();
    for (const m of doc.querySelectorAll('meta[name], meta[property], meta[itemprop]')) {
      const name = (m.getAttribute('name') || m.getAttribute('property') || m.getAttribute('itemprop') || '')
        .trim()
        .toLowerCase();
      const content = clean(m.getAttribute('content'));
      if (!name || !content) continue;
      if (!metas.has(name)) metas.set(name, []);
      metas.get(name).push(content);
    }
    return metas;
  }

  // The first JSON-LD object that describes a work (not a breadcrumb or the site).
  const ldTypes = {
    ScholarlyArticle: 'journalArticle',
    MedicalScholarlyArticle: 'journalArticle',
    NewsArticle: 'newspaperArticle',
    ReportageNewsArticle: 'newspaperArticle',
    AnalysisNewsArticle: 'newspaperArticle',
    BlogPosting: 'blogPost',
    Book: 'book',
    Thesis: 'thesis',
    Report: 'report',
    Article: 'webpage',
    TechArticle: 'webpage',
    WebPage: 'webpage',
  };
  function jsonLd(doc) {
    const found = [];
    const visit = (node) => {
      if (!node || typeof node !== 'object') return;
      if (Array.isArray(node)) return node.forEach(visit);
      if (node['@graph']) visit(node['@graph']);
      const types = [].concat(node['@type'] || []);
      if (types.some((t) => ldTypes[t])) found.push(node);
    };
    for (const s of doc.querySelectorAll('script[type="application/ld+json"]')) {
      try {
        visit(JSON.parse(s.textContent));
      } catch (e) {
        // a broken block: the others may do
      }
    }
    // a work's own type before WebPage, which wraps it on many sites
    found.sort((a, b) => ([].concat(a['@type']).includes('WebPage') ? 1 : 0) - ([].concat(b['@type']).includes('WebPage') ? 1 : 0));
    return found[0] || null;
  }

  function ldText(v) {
    if (typeof v === 'string') return clean(v);
    if (Array.isArray(v)) return ldText(v[0]);
    if (v && typeof v === 'object') return clean(v.name || v['@value'] || '');
    return '';
  }

  // "Last, First", "First Last", or an organisation's one name.
  function creator(name, organisation) {
    name = clean(name);
    if (!name || /^https?:\/\//.test(name)) return null;
    if (organisation) return {creatorType: 'author', name};
    if (name.includes(',')) {
      const [last, ...first] = name.split(',');
      return {creatorType: 'author', lastName: clean(last), firstName: clean(first.join(','))};
    }
    const parts = name.split(' ');
    if (parts.length === 1) return {creatorType: 'author', lastName: name, firstName: ''};
    return {creatorType: 'author', firstName: parts.slice(0, -1).join(' '), lastName: parts[parts.length - 1]};
  }

  function ldCreators(v) {
    return []
      .concat(v || [])
      .map((a) => (typeof a === 'string' ? creator(a) : a && creator(a.name, [].concat(a['@type']).includes('Organization'))))
      .filter(Boolean);
  }

  // 2023/01/19, 2023-01-19T10:00:00Z, 2023 -> 2023-01-19, 2023
  function date(s) {
    s = clean(s);
    const m = s.match(/^(\d{4})(?:[-/.](\d{1,2})(?:[-/.](\d{1,2}))?)?/);
    if (!m) return s;
    return [m[1], m[2] && m[2].padStart(2, '0'), m[3] && m[3].padStart(2, '0')].filter(Boolean).join('-');
  }

  function doi(s) {
    const m = clean(s).match(/10\.\d{4,9}\/[^\s"<>]+/);
    return m ? m[0].replace(/[.,;]+$/, '') : '';
  }

  function absolute(href, base) {
    try {
      return new URL(href, base).href;
    } catch (e) {
      return '';
    }
  }

  function ourNotesExtract(doc, pageUrl) {
    const title0 = clean(doc.title);
    const pdfAddress = /\.pdf([?#]|$)/i.test(new URL(pageUrl).pathname + '') || ourNotesArxivAbs(pageUrl);
    if (doc.contentType === 'application/pdf' || pdfAddress) {
      return {item: null, pdfUrl: pageUrl, isPdf: true, title: title0};
    }
    const metas = metaTags(doc);
    const one = (...names) => {
      for (const n of names) if (metas.has(n)) return metas.get(n)[0];
      return '';
    };
    const all = (...names) => {
      for (const n of names) if (metas.has(n)) return metas.get(n);
      return [];
    };
    const ld = jsonLd(doc);
    const canonical = doc.querySelector('link[rel="canonical"]');
    const url = (canonical && absolute(canonical.getAttribute('href'), pageUrl)) || pageUrl;

    const item = {};
    const arxiv =
      one('citation_arxiv_id') ||
      ((pageUrl.match(/arxiv\.org\/(?:abs|pdf|html)\/([^?#]+?)(?:v\d+)?(?:\.pdf)?(?:[?#]|$)/) || [])[1] || '');

    // the type
    if (arxiv) item.itemType = 'preprint';
    else if (one('citation_conference_title')) item.itemType = 'conferencePaper';
    else if (one('citation_dissertation_institution')) item.itemType = 'thesis';
    else if (one('citation_technical_report_institution')) item.itemType = 'report';
    else if (one('citation_book_title', 'citation_inbook_title')) item.itemType = 'bookSection';
    else if (one('citation_journal_title')) item.itemType = 'journalArticle';
    else if (one('citation_isbn') && one('citation_title')) item.itemType = 'book';
    else if (ld) item.itemType = [].concat(ld['@type']).map((t) => ldTypes[t]).find(Boolean) || 'webpage';
    else item.itemType = 'webpage';

    item.title =
      one('citation_title', 'dc.title', 'dcterms.title') ||
      (ld && ldText(ld.headline || ld.name)) ||
      one('og:title', 'twitter:title') ||
      title0;

    let creators = all('citation_author').map((a) => creator(a)).filter(Boolean);
    if (!creators.length && ld) creators = ldCreators(ld.author || ld.creator);
    if (!creators.length) creators = all('dc.creator', 'dcterms.creator').map((a) => creator(a)).filter(Boolean);
    if (!creators.length)
      creators = all('author', 'article:author', 'parsely-author', 'sailthru.author')
        .flatMap((a) => a.split(/\s*;\s*/))
        .map((a) => creator(a))
        .filter(Boolean);
    item.creators = creators;

    const published =
      one('citation_publication_date', 'citation_date', 'citation_online_date', 'citation_cover_date') ||
      one('dc.date', 'dcterms.date', 'dcterms.issued', 'prism.publicationdate') ||
      (ld && ldText(ld.datePublished || ld.dateCreated)) ||
      one('article:published_time', 'og:published_time', 'date', 'pubdate');
    if (published) item.date = date(published);

    item.url = url;
    const abstract =
      one('citation_abstract', 'dc.description', 'dcterms.abstract', 'dcterms.description') ||
      (ld && ldText(ld.abstract || ld.description)) ||
      one('description', 'og:description', 'twitter:description');
    if (abstract) item.abstractNote = abstract;

    const language = one('citation_language', 'dc.language', 'dcterms.language') || (doc.documentElement && doc.documentElement.lang) || '';
    if (language) item.language = clean(language);

    let foundDoi = doi(one('citation_doi', 'prism.doi', 'bepress_citation_doi'));
    if (!foundDoi) for (const v of all('dc.identifier', 'dcterms.identifier')) if ((foundDoi = doi(v))) break;
    if (!foundDoi && ld) foundDoi = doi(ldText(ld.identifier) || ld.sameAs || '');
    if (foundDoi) item.DOI = foundDoi;

    const site = one('og:site_name', 'citation_publisher', 'dc.publisher') || (ld && ldText(ld.publisher)) || '';
    const publisher = one('citation_publisher', 'dc.publisher', 'dcterms.publisher') || (ld && ldText(ld.publisher)) || '';
    const first = one('citation_firstpage'), last = one('citation_lastpage');
    const pages = first && last && first !== last ? `${first}-${last}` : first;
    const isbn = one('citation_isbn', 'book:isbn') || (ld && ldText(ld.isbn));
    switch (item.itemType) {
      case 'journalArticle':
        item.publicationTitle = one('citation_journal_title', 'prism.publicationname');
        item.journalAbbreviation = one('citation_journal_abbrev');
        item.volume = one('citation_volume', 'prism.volume');
        item.issue = one('citation_issue', 'prism.number');
        item.pages = pages;
        item.ISSN = one('citation_issn', 'prism.issn', 'prism.eissn');
        break;
      case 'conferencePaper':
        item.proceedingsTitle = one('citation_conference_title', 'citation_journal_title');
        item.conferenceName = one('citation_conference_title');
        item.publisher = publisher;
        item.pages = pages;
        item.ISBN = isbn;
        break;
      case 'bookSection':
        item.bookTitle = one('citation_book_title', 'citation_inbook_title');
        item.publisher = publisher;
        item.pages = pages;
        item.ISBN = isbn;
        break;
      case 'book':
        item.publisher = publisher;
        item.ISBN = isbn;
        break;
      case 'thesis':
        item.university = one('citation_dissertation_institution');
        break;
      case 'report':
        item.institution = one('citation_technical_report_institution');
        item.reportNumber = one('citation_technical_report_number');
        break;
      case 'preprint':
        item.repository = 'arXiv';
        item.archiveID = `arXiv:${arxiv}`;
        item.url = `https://arxiv.org/abs/${arxiv}`;
        if (!item.DOI) item.DOI = `10.48550/arXiv.${arxiv}`;
        break;
      case 'newspaperArticle':
        item.publicationTitle = site;
        item.section = one('article:section');
        break;
      case 'blogPost':
        item.blogTitle = site;
        break;
      default:
        item.websiteTitle = site;
    }
    for (const k of Object.keys(item)) if (item[k] === '' || item[k] === undefined) delete item[k];

    const keywords = all('citation_keywords').flatMap((k) => k.split(/\s*[;,]\s*/)).filter(Boolean);
    if (keywords.length) item.tags = [...new Set(keywords)].slice(0, 30);

    let pdfUrl = one('citation_pdf_url', 'bepress_citation_pdf_url', 'eprints.document_url');
    if (!pdfUrl) {
      const link = doc.querySelector('link[rel="alternate"][type="application/pdf"]');
      if (link) pdfUrl = link.getAttribute('href');
    }
    if (arxiv) pdfUrl = `https://arxiv.org/pdf/${arxiv}`;
    pdfUrl = pdfUrl ? absolute(pdfUrl, pageUrl) : '';
    return {item, pdfUrl, isPdf: false, title: item.title};
  }

  // An arXiv PDF's abstract page (arxiv.org/pdf/2301.08243v2 -> arxiv.org/abs/2301.08243),
  // '' for any other address.
  function ourNotesArxivAbs(url) {
    const m = String(url).match(/^https?:\/\/(?:www\.)?arxiv\.org\/pdf\/([^?#]+?)(?:v\d+)?(?:\.pdf)?(?:[?#]|$)/i);
    return m ? `https://arxiv.org/abs/${m[1]}` : '';
  }

  global.ourNotesExtract = ourNotesExtract;
  global.ourNotesArxivAbs = ourNotesArxivAbs;
})(typeof window !== 'undefined' ? window : globalThis);
