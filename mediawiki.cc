/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "mediawiki.hh"
#include "wstring_qt.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QtXml>
#include <list>
#include "gddebug.hh"
#include "audiolink.hh"
#include "langcoder.hh"
#include "qt4x5.hh"

#include <QDir>
#include <fstream>
#include <iomanip>
#include <chrono>
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

namespace MediaWiki {

using namespace Dictionary;

namespace {

class MediaWikiDictionary: public Dictionary::Class
{
  string name;
  QString url, icon;
  QNetworkAccessManager & netMgr;
  quint32 langId;

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

};

void MediaWikiDictionary::loadIcon() throw()
{
  if ( dictionaryIconLoaded )
    return;

  if( !icon.isEmpty() )
  {
    QFileInfo fInfo(  QDir( Config::getConfigDir() ), icon );
    if( fInfo.isFile() )
      loadIconFromFile( fInfo.absoluteFilePath(), true );
  }
  if( dictionaryIcon.isNull() )
    dictionaryIcon = dictionaryNativeIcon = QIcon(":/icons/icon32_wiki.png");
  dictionaryIconLoaded = true;
}

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

  Qt4x5::Url::addQueryItem( reqUrl, "apfrom", gd::toQString( str ) );

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

class MediaWikiArticleRequest: public MediaWikiDataRequestSlots
{
  typedef std::list< std::pair< QNetworkReply *, bool > > NetReplies;
  NetReplies netReplies;
  QString url;

public:

  MediaWikiArticleRequest( wstring const & word, vector< wstring > const & alts,
                           QString const & url, QNetworkAccessManager & mgr,
                           Class * dictPtr_ );

  virtual void cancel();

private:

  void addQuery( QNetworkAccessManager & mgr, wstring const & word );

  virtual void requestFinished( QNetworkReply * );
  Class * dictPtr;
};

void MediaWikiArticleRequest::cancel()
{
  finish();
}

MediaWikiArticleRequest::MediaWikiArticleRequest( wstring const & str,
                                                  vector< wstring > const & alts,
                                                  QString const & url_,
                                                  QNetworkAccessManager & mgr,
                                                  Class * dictPtr_ ):
  url( url_ ), dictPtr( dictPtr_ )
{
  connect( &mgr, SIGNAL( finished( QNetworkReply * ) ),
           this, SLOT( requestFinished( QNetworkReply * ) ),
           Qt::QueuedConnection );
  
  addQuery(  mgr, str );

  for( unsigned x = 0; x < alts.size(); ++x )
    addQuery( mgr, alts[ x ] );
}

void MediaWikiArticleRequest::addQuery( QNetworkAccessManager & mgr,
                                        wstring const & str )
{
  gdDebug( "MediaWiki: requesting article %s\n", gd::toQString( str ).toUtf8().data() );

  QUrl reqUrl( url + "/api.php?action=parse&prop=text|revid&format=xml&redirects" );

  Qt4x5::Url::addQueryItem( reqUrl, "page", gd::toQString( str ) );

  QNetworkReply * netReply = mgr.get( QNetworkRequest( reqUrl ) );
  netReply->setProperty("article", gd::toQString(str));

#ifndef QT_NO_OPENSSL

  connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
           netReply, SLOT( ignoreSslErrors() ) );

#endif

  netReplies.push_back( std::make_pair( netReply, false ) );
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

  QDir outputDir = QDir::home();
  outputDir.cd("goldendict-debugging");
  const QString outputFilePath = outputDir.filePath(
              "timeOut_" + QTime::currentTime().toString("hh-mm-ss") + ".txt");
  static std::ofstream timeOut(outputFilePath.toStdString());

  decltype(Clock::now()) timeBegin;
  decltype(timeBegin) timeEnd;
  const auto printTime = [&](const char * desc) {
        timeOut << std::fixed << std::setprecision(6) << std::chrono::duration_cast<std::chrono::microseconds>(timeEnd - timeBegin).count() / 1000.0 << " - " << desc << '\n';
  };

  for( ; netReplies.size() && netReplies.front().second; netReplies.pop_front() )
  {
    QNetworkReply * netReply = netReplies.front().first;

    timeOut << "Processing article \"" << netReply->property("article").toString().toStdString() << "\"\n";
    timeBegin = Clock::now();

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
          timeEnd = Clock::now();
          printTime("after setContent");
          timeBegin = Clock::now();

        QDomNode parseNode = dd.namedItem( "api" ).namedItem( "parse" );
  
        if ( !parseNode.isNull() && parseNode.toElement().attribute( "revid" ) != "0" )
        {
          QDomNode textNode = parseNode.namedItem( "text" );
  
          if ( !textNode.isNull() )
          {
            QString articleString = textNode.toElement().text();

            timeEnd = Clock::now();
            printTime("before replacements");
            timeBegin = Clock::now();

            // Replace all ":" in links, remove '#' part in links to other articles
            int pos = 0;
            QRegExp regLinks( "<a\\s+href=\"/([^\"]+)\"" );
            for( ; ; )
            {
              pos = regLinks.indexIn( articleString, pos );
              if( pos < 0 )
                break;
              QString link = regLinks.cap( 1 );

              if( link.indexOf( "://" ) >= 0 )
              {
                // External link
                pos += regLinks.cap().size();
                continue;
              }

              if( link.indexOf( ':' ) >= 0 )
                link.replace( ':', "%3A" );

              int n = link.indexOf( '#', 1 );
              if( n > 0 )
              {
                QString anchor = link.mid( n + 1 ).replace( '_', "%5F" );
                link.truncate( n );
                link += QString( "?gdanchor=%1" ).arg( anchor );
              }

              QString newLink = QString( "<a href=\"/%1\"" ).arg( link );
              articleString.replace( pos, regLinks.cap().size(), newLink );
              pos += newLink.size();
            }

            timeEnd = Clock::now();
            printTime("after regLinks");
            timeBegin = Clock::now();

            QUrl wikiUrl( url );
            wikiUrl.setPath( "/" );
  
            // Update any special index.php pages to be absolute
            Qt4x5::Regex::replace( articleString, "<a\\shref=\"(/(\\w*/)*index.php\\?)",
                                   QString( "<a href=\"%1\\1" ).arg( wikiUrl.toString() ),
                                   Qt::CaseSensitive, true );

            timeEnd = Clock::now();
            printTime("after index.php absolute");
            timeBegin = Clock::now();

            // audio tag
            QRegExp reg1( "<audio\\s.+</audio>", Qt::CaseInsensitive, QRegExp::RegExp2 );
            reg1.setMinimal( true );
            QRegExp reg2( "<source\\s+src=\"([^\"]+)", Qt::CaseInsensitive );
            pos = 0;
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

            timeEnd = Clock::now();
            printTime("after reg1 and reg2");
            timeBegin = Clock::now();

            // audio url
            Qt4x5::Regex::replace( articleString, "<a\\s+href=\"(//upload\\.wikimedia\\.org/wikipedia/commons/[^\"'&]*\\.ogg)",
                                   QString::fromStdString( addAudioLink( string( "\"" ) + wikiUrl.scheme().toStdString() + ":\\1\"",
                                                                         this->dictPtr->getId() ) + "<a href=\"" + wikiUrl.scheme().toStdString() + ":\\1" ) );

            timeEnd = Clock::now();
            printTime("after audio url");
            timeBegin = Clock::now();

            // Add url scheme to image source urls
            articleString.replace( " src=\"//", " src=\"" + wikiUrl.scheme() + "://" );
            //fix src="/foo/bar/Baz.png"
            articleString.replace( "src=\"/", "src=\"" + wikiUrl.toString() );

            timeEnd = Clock::now();
            printTime("after plain text replacements");
            timeBegin = Clock::now();

            // Replace the href="/foo/bar/Baz" to just href="Baz".
            Qt4x5::Regex::replace( articleString, "<a\\shref=\"/([\\w\\.]*/)*",
                                   "<a href=\"", Qt::CaseSensitive, true );

            timeEnd = Clock::now();
            printTime("after href=\"/foo/bar/Baz\" -> href=\"Baz\"");
            timeBegin = Clock::now();

            //fix audio
            // For some reason QRegExp works faster than QRegularExpression in the replacement below.
            articleString.replace( QRegExp( "<button\\s+[^>]*(upload\\.wikimedia\\.org/wikipedia/commons/[^\"'&]*\\.ogg)[^>]*>\\s*<[^<]*</button>"),
                                            QString::fromStdString(addAudioLink( string( "\"" ) + wikiUrl.scheme().toStdString() + "://\\1\"", this->dictPtr->getId() ) +
                                            "<a href=\"" + wikiUrl.scheme().toStdString() + "://\\1\"><img src=\"qrcx://localhost/icons/playsound.png\" border=\"0\" alt=\"Play\"></a>" ) );
            timeEnd = Clock::now();
            printTime("after fix audio");
            timeBegin = Clock::now();

            // In those strings, change any underscores to spaces
            for( ; ; )
            {
              QString before = articleString;
              Qt4x5::Regex::replace( articleString, "<a href=\"([^/:\">#]*)_", "<a href=\"\\1 " );
  
              if ( articleString == before )
                break;
            }

            timeEnd = Clock::now();
            printTime("after underscores to spaces");
            timeBegin = Clock::now();

            //fix file: url
            Qt4x5::Regex::replace( articleString, "<a\\s+href=\"([^:/\"]*file%3A[^/\"]+\")",
                                   QString( "<a href=\"%1/index.php?title=\\1" ).arg( url ),
                                   Qt::CaseInsensitive );

            timeEnd = Clock::now();
            printTime("after fix file: url (finished replacements)");
            timeBegin = Clock::now();

            QByteArray articleBody = articleString.toUtf8();
  
            articleBody.prepend( dictPtr->isToLanguageRTL() ? "<div class=\"mwiki\" dir=\"rtl\">" :
                                                              "<div class=\"mwiki\">" );
            articleBody.append( "</div>" );
  
            Mutex::Lock _( dataMutex );

            size_t prevSize = data.size();
            
            data.resize( prevSize + articleBody.size() );
  
            memcpy( &data.front() + prevSize, articleBody.data(), articleBody.size() );
  
            hasAnyData = true;

            updated = true;
          }
        }
      }
      GD_DPRINTF( "done.\n" );
    }
    else
      setErrorString( netReply->errorString() );

    disconnect( netReply, 0, 0, 0 );
    netReply->deleteLater();

    timeEnd = Clock::now();
    printTime("loop iteration end");
    timeBegin = Clock::now();
  }

  if ( netReplies.empty() )
    finish();
  else
  if ( updated )
    update();

  timeEnd = Clock::now();
  printTime("after signals");
  timeBegin = Clock::now();

}

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
    return new MediaWikiArticleRequest( word, alts, url, netMgr, this );
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
