/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "mediawiki.hh"
#include "wstring_qt.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QtXml>
#include <algorithm>
#include <list>
#include <queue>
#include <memory>
#include "gddebug.hh"
#include "audiolink.hh"
#include "langcoder.hh"
#include "qt4x5.hh"

#if IS_QT_5
#include <QRegularExpression>
#endif

namespace MediaWiki {

using namespace Dictionary;

namespace {

class MediaWikiFactory;

class MediaWikiDictionary: public Dictionary::Class
{
  string name;
  QString url, icon;
  QNetworkAccessManager & netMgr;
  quint32 langId;
  std::auto_ptr< MediaWikiFactory > factory;

public:

  MediaWikiDictionary( string const & id, string const & name_,
                       QString const & url_,
                       QString const & icon_,
                       QNetworkAccessManager & netMgr_ ):
    Dictionary::Class( id, vector< string >() ),
    name( name_ ),
    url( url_ ),
    icon( icon_ ),
    netMgr( netMgr_ ),
    langId( 0 )
  {
    initializeFactory();

    int n = url.indexOf( "." );
    if( n == 2 || ( n > 3 && url[ n-3 ] == '/' ) )
      langId = LangCoder::code2toInt( url.mid( n - 2, 2 ).toLatin1().data() );
  }

  virtual string getName() throw()
  { return name; }

  virtual map< Property, string > getProperties() throw()
  { return map< Property, string >(); }

  virtual unsigned long getArticleCount() throw()
  { return 0; }

  virtual unsigned long getWordCount() throw()
  { return 0; }

  virtual sptr< WordSearchRequest > prefixMatch( wstring const &,
                                                 unsigned long maxResults ) throw( std::exception );

  virtual sptr< DataRequest > getArticle( wstring const &, vector< wstring > const & alts,
                                          wstring const & )
    throw( std::exception );

  virtual quint32 getLangFrom() const
  { return langId; }

  virtual quint32 getLangTo() const
  { return langId; }

protected:

  virtual void loadIcon() throw();

private:

  void initializeFactory();
};

class MediaWikiWordSearchRequest: public MediaWikiWordSearchRequestSlots
{
  sptr< QNetworkReply > netReply;
  bool livedLongEnough; // Indicates that the request has lived long enough
                        // to be destroyed prematurely. Used to prevent excessive
                        // network loads when typing search terms rapidly.
  bool isCancelling;

public:

  MediaWikiWordSearchRequest( wstring const &,
                              QString const & url, QNetworkAccessManager & mgr );

  ~MediaWikiWordSearchRequest();

  virtual void cancel();

protected:

  virtual void timerEvent( QTimerEvent * );

private:

  virtual void downloadFinished();
};

MediaWikiWordSearchRequest::MediaWikiWordSearchRequest( wstring const & str,
                                                        QString const & url,
                                                        QNetworkAccessManager & mgr ):
  livedLongEnough( false ), isCancelling( false )
{
  GD_DPRINTF( "request begin\n" );
  QUrl reqUrl( url + "/api.php?action=query&list=allpages&aplimit=40&format=xml" );

#if IS_QT_5
  Qt4x5::Url::addQueryItem( reqUrl, "apfrom", gd::toQString( str ).replace( '+', "%2B" ) );
#else
  reqUrl.addEncodedQueryItem( "apfrom", QUrl::toPercentEncoding( gd::toQString( str ) ) );
#endif

  netReply = mgr.get( QNetworkRequest( reqUrl ) );

  connect( netReply.get(), SIGNAL( finished() ),
           this, SLOT( downloadFinished() ) );

#ifndef QT_NO_OPENSSL

  connect( netReply.get(), SIGNAL( sslErrors( QList< QSslError > ) ),
           netReply.get(), SLOT( ignoreSslErrors() ) );

#endif

  // We start a timer to postpone early destruction, so a rapid type won't make
  // unnecessary network load
  startTimer( 200 );
}

void MediaWikiWordSearchRequest::timerEvent( QTimerEvent * ev )
{
  killTimer( ev->timerId() );
  livedLongEnough = true;

  if ( isCancelling )
    finish();
}

MediaWikiWordSearchRequest::~MediaWikiWordSearchRequest()
{
  GD_DPRINTF( "request end\n" );
}

void MediaWikiWordSearchRequest::cancel()
{
  // We either finish it in place, or in the timer handler
  isCancelling = true;

  if ( netReply.get() )
    netReply.reset();

  if ( livedLongEnough )
  {
    finish();
  }
  else
  {
    GD_DPRINTF("not long enough\n" );
  }
}

void MediaWikiWordSearchRequest::downloadFinished()
{
  if ( isCancelling || isFinished() ) // Was cancelled
    return;

  if ( netReply->error() == QNetworkReply::NoError )
  {
    QDomDocument dd;

    QString errorStr;
    int errorLine, errorColumn;

    if ( !dd.setContent( netReply.get(), false, &errorStr, &errorLine, &errorColumn  ) )
    {
      setErrorString( QString( tr( "XML parse error: %1 at %2,%3" ).
                               arg( errorStr ).arg( errorLine ).arg( errorColumn ) ) );
    }
    else
    {
      QDomNode pages = dd.namedItem( "api" ).namedItem( "query" ).namedItem( "allpages" );

      if ( !pages.isNull() )
      {
        QDomNodeList nl = pages.toElement().elementsByTagName( "p" );

        Mutex::Lock _( dataMutex );

        for( Qt4x5::Dom::size_type x = 0; x < nl.length(); ++x )
          matches.push_back( gd::toWString( nl.item( x ).toElement().attribute( "title" ) ) );
      }
    }
    GD_DPRINTF( "done.\n" );
  }
  else
    setErrorString( netReply->errorString() );

  finish();
}

class RootBasedUrlFixer
{
public:
  /// Replaces all ":" in links, removes '#' part in links to other articles.
  static QString fixedArticle( const QString & article );

private:
  static void fixUrl( QString & url );
};

QString RootBasedUrlFixer::fixedArticle( const QString & article )
{
  QString result;
  int pos = 0;
  const QRegExp linkPrefix( "<a\\s+href=\"/" );
  for( ; ; )
  {
    int nextPos = linkPrefix.indexIn( article, pos );
    if( nextPos < 0 )
      break;
    nextPos += linkPrefix.matchedLength();
    result += article.midRef( pos, nextPos - pos );
    pos = nextPos;

    nextPos = article.indexOf( '"', pos );
    if( nextPos < 0 )
    {
      gdWarning( "Unterminated link in a MediaWiki article." );
      break;
    }
    QString url = article.mid( pos, nextPos - pos );
    pos = nextPos;

    fixUrl( url );
    result += url;
  }
  if( pos == 0 )
    return article; // no links -> article remains unchanged.
  result += article.midRef( pos );

  return result;
}

void RootBasedUrlFixer::fixUrl( QString & url )
{
  if( url.indexOf( "://" ) >= 0 )
    return; // External link

  if( url.indexOf( ':' ) >= 0 )
    url.replace( ':', "%3A" );

  const int n = url.indexOf( '#', 1 );
  if( n > 0 )
  {
    QString anchor = url.mid( n + 1 ).replace( '_', "%5F" );
    url.truncate( n );
    url += "?gdanchor=";
    url += anchor;
  }
}

void underscoresToSpacesInLinks( QString & articleString )
{
#if IS_QT_5
  // This implementation outperforms the alternative code below
  // by an order of magnitude, but does not compile with Qt4.
  const QRegularExpression linkRegex( "<a\\s+href=\"[^/:\">#]+" );
  QRegularExpressionMatchIterator it = linkRegex.globalMatch( articleString );
  while( it.hasNext() )
  {
    const QRegularExpressionMatch match = it.next();
    std::replace( articleString.begin() + match.capturedStart(),
                  articleString.begin() + match.capturedEnd(),
                  '_', ' ' );
  }
#else
  for( ; ; )
  {
    QString before = articleString;
    Qt4x5::Regex::replace( articleString, "<a href=\"([^/:\">#]*)_", "<a href=\"\\1 " );
    if( articleString == before )
      break;
  }
#endif
}

/// @return The wiki word inside the link that contains @p linkDistinction
/// or an empty string if @p article does not contain such a link.
std::wstring findWikiLink( QString const & article, QString const & linkDistinction )
{
  const int distinctionPosition = article.indexOf( linkDistinction );
  if( distinctionPosition >= 0 )
  {
    const int linkPosition = article.lastIndexOf( QRegExp( "[<>]" ), distinctionPosition );
    const QString linkForepart = article.mid( linkPosition,
                                              distinctionPosition - linkPosition );
    const QRegExp linkPattern( "<a href=\"/wiki/([^\"]+)\".*" );
    if( linkPattern.exactMatch( linkForepart ) )
      return gd::toWString( linkPattern.cap( 1 ) );
  }
  return std::wstring();
}

class MediaWikiArticleRequest: public MediaWikiDataRequestSlots
{
public:

  /// None of the data members in this struct may be null.
  struct InitData
  {
    QString url;
    QNetworkAccessManager * netMgr;
    Class * dictPtr;
  };

  explicit MediaWikiArticleRequest( InitData const & data );

  void addQuery( wstring const & word ) { doAddQuery( word ); }

  virtual void cancel();

protected:

  QNetworkReply * createQuery( wstring const & word );

  virtual QNetworkReply const * doAddQuery( wstring const & word );

  virtual bool preprocessArticle( QString & articleString )
  {
    Q_UNUSED( articleString )
    return true;
  }

  typedef std::list< std::pair< QNetworkReply *, bool > > NetReplies;
  NetReplies netReplies;
  Class * const dictPtr;

private:

  virtual void replyHandled( QNetworkReply const * reply, bool textFound )
  {
    Q_UNUSED( reply )
    Q_UNUSED( textFound )
  }

  void processArticle( QString & articleString ) const;
  void appendArticleToData( QString const & articleString );

  virtual void requestFinished( QNetworkReply * );

  const QString url;
  QNetworkAccessManager & netMgr;
};

void MediaWikiArticleRequest::cancel()
{
  finish();
}

MediaWikiArticleRequest::MediaWikiArticleRequest( InitData const & data ):
  dictPtr( data.dictPtr ), url( data.url ), netMgr( *data.netMgr )
{
  connect( &netMgr, SIGNAL( finished( QNetworkReply * ) ),
           this, SLOT( requestFinished( QNetworkReply * ) ),
           Qt::QueuedConnection );
}

QNetworkReply * MediaWikiArticleRequest::createQuery( wstring const & str )
{
  Q_ASSERT( !isFinished() && "Finished request should not make queries!" );

  gdDebug( "MediaWiki: requesting article %s\n", gd::toQString( str ).toUtf8().data() );

  QUrl reqUrl( url + "/api.php?action=parse&prop=text|revid&format=xml&redirects" );

#if IS_QT_5
  Qt4x5::Url::addQueryItem( reqUrl, "page", gd::toQString( str ).replace( '+', "%2B" ) );
#else
  reqUrl.addEncodedQueryItem( "page", QUrl::toPercentEncoding( gd::toQString( str ) ) );
#endif

  QNetworkReply * netReply = netMgr.get( QNetworkRequest( reqUrl ) );
  
#ifndef QT_NO_OPENSSL

  connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
           netReply, SLOT( ignoreSslErrors() ) );

#endif

  return netReply;
}

QNetworkReply const * MediaWikiArticleRequest::doAddQuery( wstring const & word )
{
  QNetworkReply * const reply = createQuery( word );
  netReplies.push_back( std::make_pair( reply, false ) );
  return reply;
}

void MediaWikiArticleRequest::processArticle( QString & articleString ) const
{
  articleString = RootBasedUrlFixer::fixedArticle( articleString );

  QUrl wikiUrl( url );
  wikiUrl.setPath( "/" );

  // Update any special index.php pages to be absolute
  Qt4x5::Regex::replace( articleString, "<a\\shref=\"(/(\\w*/)*index.php\\?)",
                         QString( "<a href=\"%1\\1" ).arg( wikiUrl.toString() ),
                         Qt::CaseSensitive, true );

  // audio tag
  QRegExp reg1( "<audio\\s.+</audio>", Qt::CaseInsensitive, QRegExp::RegExp2 );
  reg1.setMinimal( true );
  QRegExp reg2( "<source\\s+src=\"([^\"]+)", Qt::CaseInsensitive );
  int pos = 0;
  for( ; ; )
  {
    pos = reg1.indexIn( articleString, pos );
    if( pos >= 0 )
    {
      QString tag = reg1.cap();
      if( reg2.indexIn( tag ) >= 0 )
      {
        QString ref = reg2.cap( 1 );
        QString audio_url = "<a href=\"" + ref
                            + "\"><img src=\"qrcx://localhost/icons/playsound.png\" border=\"0\" align=\"absmiddle\" alt=\"Play\"/></a>";
        articleString.replace( pos, tag.length(), audio_url );
      }
      pos += 1;
    }
    else
      break;
  }

  // audio url
  Qt4x5::Regex::replace( articleString, "<a\\s+href=\"(//upload\\.wikimedia\\.org/wikipedia/commons/[^\"'&]*\\.ogg)",
                         QString::fromStdString( addAudioLink( string( "\"" ) + wikiUrl.scheme().toStdString() + ":\\1\"",
                                                               this->dictPtr->getId() ) + "<a href=\"" + wikiUrl.scheme().toStdString() + ":\\1" ) );

  // Add url scheme to image source urls
  articleString.replace( " src=\"//", " src=\"" + wikiUrl.scheme() + "://" );
  //fix src="/foo/bar/Baz.png"
  articleString.replace( "src=\"/", "src=\"" + wikiUrl.toString() );

  // Remove the /wiki/ prefix from links
  // For some reason QRegExp works faster than QRegularExpression in the replacement below.
  articleString.replace( QRegExp( "<a\\shref=\"/wiki/" ), "<a href=\"" );

  //fix audio
  // For some reason QRegExp works faster than QRegularExpression in the replacement below.
  articleString.replace( QRegExp( "<button\\s+[^>]*(upload\\.wikimedia\\.org/wikipedia/commons/[^\"'&]*\\.ogg)[^>]*>\\s*<[^<]*</button>"),
                                  QString::fromStdString(addAudioLink( string( "\"" ) + wikiUrl.scheme().toStdString() + "://\\1\"", this->dictPtr->getId() ) +
                                  "<a href=\"" + wikiUrl.scheme().toStdString() + "://\\1\"><img src=\"qrcx://localhost/icons/playsound.png\" border=\"0\" alt=\"Play\"></a>" ) );

  underscoresToSpacesInLinks( articleString );

  //fix file: url
  Qt4x5::Regex::replace( articleString, "<a\\s+href=\"([^:/\"]*file%3A[^/\"]+\")",
                         QString( "<a href=\"%1/index.php?title=\\1" ).arg( url ),
                         Qt::CaseInsensitive );
}

void MediaWikiArticleRequest::appendArticleToData( QString const & articleString )
{
  QByteArray articleBody = articleString.toUtf8();
  articleBody.prepend( dictPtr->isToLanguageRTL() ? "<div class=\"mwiki\" dir=\"rtl\">" :
                                                    "<div class=\"mwiki\">" );
  articleBody.append( "</div>" );

  Mutex::Lock _( dataMutex );
  size_t prevSize = data.size();
  data.resize( prevSize + articleBody.size() );
  memcpy( &data.front() + prevSize, articleBody.data(), articleBody.size() );
  hasAnyData = true;
}

void MediaWikiArticleRequest::requestFinished( QNetworkReply * r )
{
  GD_DPRINTF( "Finished.\n" );

  if ( isFinished() ) // Was cancelled
    return;

  // Find this reply

  bool found = false;
  
  for( NetReplies::iterator i = netReplies.begin(); i != netReplies.end(); ++i )
  {
    if ( i->first == r )
    {
      i->second = true; // Mark as finished
      found = true;
      break;
    }
  }

  if ( !found )
  {
    // Well, that's not our reply, don't do anything
    return;
  }
  
  bool updated = false;

  while( netReplies.size() && netReplies.front().second )
  {
    QNetworkReply * netReply = netReplies.front().first;
    netReplies.pop_front();

    bool textFound = false;
    
    if ( netReply->error() == QNetworkReply::NoError )
    {
      QDomDocument dd;
  
      QString errorStr;
      int errorLine, errorColumn;
  
      if ( !dd.setContent( netReply, false, &errorStr, &errorLine, &errorColumn  ) )
      {
        setErrorString( QString( tr( "XML parse error: %1 at %2,%3" ).
                                 arg( errorStr ).arg( errorLine ).arg( errorColumn ) ) );
      }
      else
      {
        QDomNode parseNode = dd.namedItem( "api" ).namedItem( "parse" );
  
        if ( !parseNode.isNull() && parseNode.toElement().attribute( "revid" ) != "0" )
        {
          QDomNode textNode = parseNode.namedItem( "text" );
  
          if ( !textNode.isNull() )
          {
            QString articleString = textNode.toElement().text();
            textFound = true;
            if( preprocessArticle( articleString ) )
            {
              processArticle( articleString );
              appendArticleToData( articleString );
              updated = true;
            }
          }
        }
      }
      GD_DPRINTF( "done.\n" );
    }
    else
      setErrorString( netReply->errorString() );

    replyHandled( netReply, textFound );

    disconnect( netReply, 0, 0, 0 );
    netReply->deleteLater();
  }

  if ( netReplies.empty() )
    finish();
  else
  if ( updated )
    update();
}

class MediaWikiFactory
{
  Q_DISABLE_COPY( MediaWikiFactory )
public:

  MediaWikiFactory() {}
  virtual ~MediaWikiFactory() {}

  virtual QIcon defaultIcon() const { return QIcon( ":/icons/icon32_wiki.png" ); }

  typedef MediaWikiArticleRequest::InitData InitData;

  virtual sptr< MediaWikiArticleRequest > articleRequest( InitData const & data ) const
  {
    return new MediaWikiArticleRequest( data );
  }
};

class FandomArticleRequest: public MediaWikiArticleRequest
{
public:

  explicit FandomArticleRequest( InitData const & data ):
    MediaWikiArticleRequest( data ) {}

protected:

  virtual bool preprocessArticle( QString & articleString );
};

bool FandomArticleRequest::preprocessArticle( QString & articleString )
{
  // Lazy loading does not work in goldendict -> display these images
  // by switching to the simpler alternative format under <noscript> tag.
  Qt4x5::Regex::replace( articleString,
                         "<img\\s[^>]+lzy lzyPlcHld[^>]+>\\s*<noscript>\\s*(<img\\s[^<]+)</noscript>",
                         "\\1" );

  // audio url
  // For some reason QRegExp works faster than QRegularExpression in the replacement below.
  articleString.replace(
        QRegExp( "<a href=(\"https://vignette.wikia.nocookie.net/[^\"]+\\.ogg)(/revision/latest)?(\\?cb=\\d+)?\"" ),
        QString::fromStdString( addAudioLink( "\\1\"", this->dictPtr->getId() ) + "<a href=\\1\"" ) );

  // Remove absolute height from scrollbox lines to ensure that everything inside
  // the scrollable container is visible and does not overlap the contents below.
  // For some reason QRegExp works faster than QRegularExpression in the replacement below.
  articleString.replace( QRegExp( "(class=\"scrollbox\"[^\\n]*[^-])height:\\d+px;" ),
                         "\\1" );

  return true;
}

class FandomFactory: public MediaWikiFactory
{
public:

  virtual QIcon defaultIcon() const { return QIcon( ":/icons/icon32_fandom.png" ); }

  virtual sptr< MediaWikiArticleRequest > articleRequest( InitData const & data ) const
  {
    return new FandomArticleRequest( data );
  }
};

/// This class searches for redirectLinkDistinction in one of the links
/// inside the article text. If found, displays the definition of the word that
/// is this link's target instead of the original article text.
class RedirectingArticleRequest: public FandomArticleRequest
{
public:

  explicit RedirectingArticleRequest( InitData const & data,
                                      QString const & redirectLinkDistinction_ ):
    FandomArticleRequest( data ), redirectLinkDistinction( redirectLinkDistinction_ )
  {}

protected:

  virtual bool preprocessArticle( QString & articleString );

  void prependQuery( wstring const & str );

private:

  const QString redirectLinkDistinction;
};

bool RedirectingArticleRequest::preprocessArticle( QString & articleString )
{
  if( !redirectLinkDistinction.isEmpty() )
  {
    const std::wstring wikiWord = findWikiLink( articleString, redirectLinkDistinction );
    if( !wikiWord.empty() )
    {
      // Found our link distintion -> redirect.
      prependQuery( wikiWord );
      return false;
    }
  }

  return FandomArticleRequest::preprocessArticle( articleString );
}

void RedirectingArticleRequest::prependQuery( wstring const & str )
{
  netReplies.push_front( std::make_pair( createQuery( str ), false ) );
}

/// If the selected word does not end with preferableSuffix, this class requests
/// the definition of the word with preferableSuffix appended. If this modified
/// request fails, the definition of the original word is requested.
class SuffixAddingArticleRequest: public RedirectingArticleRequest
{
public:

  explicit SuffixAddingArticleRequest( InitData const & data,
                                       QString const & redirectLinkDistinction_,
                                       std::wstring const & preferableSuffix_ ):
    RedirectingArticleRequest( data, redirectLinkDistinction_ ),
    preferableSuffix( preferableSuffix_ )
  {}

protected:

  virtual QNetworkReply const * doAddQuery( wstring const & word );

private:

  virtual void replyHandled( QNetworkReply const * reply, bool textFound );

  const std::wstring preferableSuffix;

  struct Replacement
  {
    QNetworkReply const * reply;
    std::wstring originalWord;
  };
  /// The relative QNetworkReply order in replacements is the same as in netReplies.
  std::queue< Replacement > replacements;
};

QNetworkReply const * SuffixAddingArticleRequest::doAddQuery( wstring const & word )
{
  if( std::equal( word.end() - preferableSuffix.size(), word.end(), preferableSuffix.begin() ) )
    return RedirectingArticleRequest::doAddQuery( word );

  // Try the corresponding preferable article first.
  QNetworkReply const * const reply = RedirectingArticleRequest::doAddQuery( word + preferableSuffix );
  Replacement replacement = { reply, word };
  replacements.push( replacement );
  return reply;
}

void SuffixAddingArticleRequest::replyHandled( QNetworkReply const * reply, bool textFound )
{
  if( replacements.empty() || reply != replacements.front().reply )
    return;
  if( !textFound )
  {
    // Couldn't load the preferable article -> try the original word instead.
    prependQuery( replacements.front().originalWord );
  }
  replacements.pop();
}

/// Ensures that Wookieepedia era icons are visible at the top of the article.
/// The most important "era icon" is the Canon or Legends indicator.
/// It is not immediately obvious whether the current article is
/// the Canon or the Legends version of the subject without this indicator.
void makeEraIconsVisible( QString & article )
{
  // For some reason QRegExp works faster than QRegularExpression in the replacement below.
  article.replace( QRegExp( "(id=\"title-eraicons\" style=\"[^\"]*)display:none;?" ),
                            "\\1" );
}

/// This class displays the default Wookieepedia tab for the requested word.
class WookieepediaArticleRequest: public FandomArticleRequest
{
public:

  explicit WookieepediaArticleRequest( InitData const & data ):
    FandomArticleRequest( data ) {}

protected:

  virtual bool preprocessArticle( QString & articleString );
};

bool WookieepediaArticleRequest::preprocessArticle( QString & articleString )
{
  if( !FandomArticleRequest::preprocessArticle( articleString ) )
    return false;
  makeEraIconsVisible( articleString );
  return true;
}

class WookieepediaFactory: public FandomFactory
{
public:

  virtual QIcon defaultIcon() const { return QIcon( ":/icons/icon32_wookieepedia.png" ); }

  virtual sptr< MediaWikiArticleRequest > articleRequest( InitData const & data ) const
  {
    return new WookieepediaArticleRequest( data );
  }
};

/// This class displays the Wookieepedia Legends definition of the requested
/// word if it exists. If there is no Legends version of the article,
/// the Wookieepedia Canon definition is displayed.
class WookieepediaLegendsArticleRequest: public SuffixAddingArticleRequest
{
public:

  explicit WookieepediaLegendsArticleRequest( InitData const & data ):
    SuffixAddingArticleRequest(
      data,

      // Detect inactive Legends tab. If found, discard the current article
      // and ask for its Legends version instead.
      "title=\"Click here for Wookieepedia&#39;s article on the Legends version of this subject.\"",

      // Before searching for the original word, send a request for the word
      // with the /Legends suffix. In case of success, this saves waiting for,
      // then parsing the Canon reply (which may contain a long article),
      // and detecting the inactive Legends tab.
      // In case of failure, the penalty is smaller: one extra network request
      // and relatively quick parsing of the missing /Legends page reply.
      L"/Legends" )
  {}

protected:

  virtual bool preprocessArticle( QString & articleString );
};

bool WookieepediaLegendsArticleRequest::preprocessArticle( QString & articleString )
{
  if( !SuffixAddingArticleRequest::preprocessArticle( articleString ) )
    return false;
  makeEraIconsVisible( articleString );
  return true;
}

class WookieepediaLegendsFactory: public WookieepediaFactory
{
public:

  virtual sptr< MediaWikiArticleRequest > articleRequest( InitData const & data ) const
  {
    return new WookieepediaLegendsArticleRequest( data );
  }
};

sptr< WordSearchRequest > MediaWikiDictionary::prefixMatch( wstring const & word,
                                                            unsigned long maxResults )
  throw( std::exception )
{
  (void) maxResults;
  if ( word.size() > 80 )
  {
    // Don't make excessively large queries -- they're fruitless anyway

    return new WordSearchRequestInstant();
  }
  else
    return new MediaWikiWordSearchRequest( word, url, netMgr );
}

sptr< DataRequest > MediaWikiDictionary::getArticle( wstring const & word,
                                                     vector< wstring > const & alts,
                                                     wstring const & )
  throw( std::exception )
{
  if ( word.size() > 80 )
  {
    // Don't make excessively large queries -- they're fruitless anyway

    return new DataRequestInstant( false );
  }
  else
  {
    const MediaWikiArticleRequest::InitData initData = { url, &netMgr, this };
    sptr< MediaWikiArticleRequest > request = factory->articleRequest( initData );

    request->addQuery( word );
    for( std::size_t i = 0; i < alts.size(); ++i )
      request->addQuery( alts[ i ] );

    return request;
  }
}

void MediaWikiDictionary::loadIcon() throw()
{
  if( dictionaryIconLoaded )
    return;

  if( !icon.isEmpty() )
  {
    QFileInfo fInfo(  QDir( Config::getConfigDir() ), icon );
    if( fInfo.isFile() )
      loadIconFromFile( fInfo.absoluteFilePath(), true );
  }
  if( dictionaryIcon.isNull() )
    dictionaryIcon = dictionaryNativeIcon = factory->defaultIcon();
  dictionaryIconLoaded = true;
}

void MediaWikiDictionary::initializeFactory()
{
  if( url.endsWith( "/starwars.wikia.com (Legends)" ) )
  {
    const int legendsSuffixLength = 10;
    url.chop( legendsSuffixLength );
    factory.reset( new WookieepediaLegendsFactory );
  }
  else if( url.endsWith( "/starwars.wikia.com" ) )
    factory.reset( new WookieepediaFactory );
  else if( url.endsWith( ".wikia.com" ) )
    factory.reset( new FandomFactory );
  else
    factory.reset( new MediaWikiFactory );
}

}

vector< sptr< Dictionary::Class > > makeDictionaries(
                                      Dictionary::Initializing &,
                                      Config::MediaWikis const & wikis,
                                      QNetworkAccessManager & mgr )
  throw( std::exception )
{
  vector< sptr< Dictionary::Class > > result;

  for( int x = 0; x < wikis.size(); ++x )
  {
    if ( wikis[ x ].enabled )
      result.push_back( new MediaWikiDictionary( wikis[ x ].id.toStdString(),
                                                 wikis[ x ].name.toUtf8().data(),
                                                 wikis[ x ].url,
                                                 wikis[ x ].icon,
                                                 mgr ) );
  }

  return result;
}

}
