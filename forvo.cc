/* This file is (c) 2008-2010 Konstantin Isakov <ikm@users.berlios.de>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "forvo.hh"
#include "wstring_qt.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QtXml>
#include <list>
#include "audiolink.hh"
#include "htmlescape.hh"
#include "country.hh"
#include "language.hh"
#include "langcoder.hh"

namespace Forvo {

using namespace Dictionary;

namespace {

class ForvoDictionary: public Dictionary::Class
{
  string name;
  QString apiKey, languageCode;
  QNetworkAccessManager & netMgr;

public:

  ForvoDictionary( string const & id, string const & name_,
                   QString const & apiKey_,
                   QString const & languageCode_,
                   QNetworkAccessManager & netMgr_ ):
    Dictionary::Class( id, vector< string >() ),
    name( name_ ),
    apiKey( apiKey_ ),
    languageCode( languageCode_ ),
    netMgr( netMgr_ )
  {
  }

  virtual string getName() throw()
  { return name; }

  virtual map< Property, string > getProperties() throw()
  { return map< Property, string >(); }

  virtual unsigned long getArticleCount() throw()
  { return 0; }

  virtual unsigned long getWordCount() throw()
  { return 0; }

  virtual QIcon getIcon() throw();

  virtual sptr< WordSearchRequest > prefixMatch( wstring const & word,
                                                 unsigned long /*maxResults*/ ) throw( std::exception )
  {
    // Dummy
    sptr< WordSearchRequestInstant > sr = new WordSearchRequestInstant;

    sr->getMatches().push_back( WordMatch( word, 1 ) );

    return sr;
  }

  virtual sptr< DataRequest > getArticle( wstring const &, vector< wstring > const & alts,
                                          wstring const & )
    throw( std::exception );
};

class ForvoArticleRequest: public ForvoDataRequestSlots
{
  typedef std::list< std::pair< sptr< QNetworkReply >, bool > > NetReplies;
  NetReplies netReplies;
  QString word, apiKey, languageCode;
  string dictionaryId;

public:

  ForvoArticleRequest( wstring const & word, vector< wstring > const & alts,
                       QString const & apiKey_,
                       QString const & languageCode_,
                       string const & dictionaryId_,
                       QNetworkAccessManager & mgr );

  virtual void cancel();

private:

  void addQuery( QNetworkAccessManager & mgr, wstring const & word );

  virtual void requestFinished( QNetworkReply * );
};

void ForvoArticleRequest::cancel()
{
  finish();
}

ForvoArticleRequest::ForvoArticleRequest( wstring const & str,
                                          vector< wstring > const & /*alts*/,
                                          QString const & apiKey_,
                                          QString const & languageCode_,
                                          string const & dictionaryId_,
                                          QNetworkAccessManager & mgr ):
  word( gd::toQString( str ) ), apiKey( apiKey_ ),
  languageCode( languageCode_ ), dictionaryId( dictionaryId_ )
{
  connect( &mgr, SIGNAL( finished( QNetworkReply * ) ),
           this, SLOT( requestFinished( QNetworkReply * ) ),
           Qt::QueuedConnection );
  
  addQuery(  mgr, str );

  // Don't do alts for now -- the api onlu allows 1000 requests a day a key
#if 0
  for( unsigned x = 0; x < alts.size(); ++x )
    addQuery( mgr, alts[ x ] );
#endif
}

void ForvoArticleRequest::addQuery( QNetworkAccessManager & mgr,
                                    wstring const & str )
{
  printf( "Requesting article %ls\n", str.c_str() );

  if ( apiKey.simplified().isEmpty() )
  {
    // Use the default api key. That's the key I have just registered myself.
    // It has a limit of 1000 requests a day, and may also get banned in the
    // future. Can't do much about it. Get your own key, it is simple.
    apiKey = "5efa5d045a16d10ad9c4705bd5d8e56a";
  }

  QUrl reqUrl = QUrl::fromEncoded(
      QString( "http://apifree.forvo.com/key/%1/format/xml/action/word-pronunciations/word/%2/language/%3" )
      .arg( apiKey ).arg( QString::fromAscii( QUrl::toPercentEncoding( gd::toQString( str ) ) ) ).arg( languageCode ).toUtf8() );

  printf( "req: %s\n", reqUrl.toEncoded().data() );

  sptr< QNetworkReply > netReply = mgr.get( QNetworkRequest( reqUrl ) );
  
  netReplies.push_back( std::make_pair( netReply, false ) );
}

void ForvoArticleRequest::requestFinished( QNetworkReply * r )
{
  printf( "Finished.\n" );

  if ( isFinished() ) // Was cancelled
    return;

  // Find this reply

  bool found = false;
  
  for( NetReplies::iterator i = netReplies.begin(); i != netReplies.end(); ++i )
  {
    if ( i->first.get() == r )
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

  for( ; netReplies.size() && netReplies.front().second; netReplies.pop_front() )
  {
    sptr< QNetworkReply > netReply = netReplies.front().first;
    
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
        printf( "%s\n", dd.toByteArray().data() );

        QDomNode items = dd.namedItem( "items" );
  
        if ( !items.isNull() )
        {
          QDomNodeList nl = items.toElement().elementsByTagName( "item" );

          if ( nl.count() )
          {
            string articleBody;

            articleBody += "<div class='forvo_headword'>";
            articleBody += Html::escape( word.toUtf8().data() );
            articleBody += "</div>";

            articleBody += "<table class=\"forvo_play\">";

            for( unsigned x = 0; x < nl.length(); ++x )
            {
              QDomElement item = nl.item( x ).toElement();

              QDomNode mp3 = item.namedItem( "pathmp3" );

              if ( !mp3.isNull() )
              {
                articleBody += "<tr>";

                QUrl url( mp3.toElement().text() );

                string ref = string( "\"" ) + url.toEncoded().data() + "\"";

                articleBody += addAudioLink( ref, dictionaryId ).c_str();

                bool isMale = ( item.namedItem( "sex" ).toElement().text().toLower() != "f" );

                QString user = item.namedItem( "username" ).toElement().text();
                QString country = item.namedItem( "country" ).toElement().text();

                string userProfile = string( "http://www.forvo.com/user/" ) +
                                     QUrl::toPercentEncoding( user ).data() + "/";

                int totalVotes = item.namedItem( "num_votes" ).toElement().text().toInt();
                int positiveVotes = item.namedItem( "num_positive_votes" ).toElement().text().toInt();
                int negativeVotes = totalVotes - positiveVotes;

                string votes;

                if ( positiveVotes || negativeVotes )
                {
                  votes += " ";

                  if ( positiveVotes )
                  {
                    votes += "<span class='forvo_positive_votes'>+";
                    votes += QByteArray::number( positiveVotes ).data();
                    votes += "</span>";
                  }

                  if ( negativeVotes )
                  {
                    if ( positiveVotes )
                      votes += " ";

                    votes += "<span class='forvo_negative_votes'>-";
                    votes += QByteArray::number( negativeVotes ).data();
                    votes += "</span>";
                  }
                }

                string addTime =
                    tr( "Added %1" ).arg( item.namedItem( "addtime" ).toElement().text() ).toUtf8().data();

                articleBody += "<td><a href=" + ref + " title=\"" + Html::escape( addTime ) + "\"><img src=\"qrcx://localhost/icons/playsound.png\" border=\"0\" alt=\"Play\"/></a></td>";
                articleBody += string( "<td>" ) + tr( "by" ).toUtf8().data() + " <a class='forvo_user' href='"
                               + userProfile + "'>"
                               + Html::escape( user.toUtf8().data() )
                               + "</a> <span class='forvo_location'>("
                               + ( isMale ? tr( "Male" ) : tr( "Female" ) ).toUtf8().data()
                               + " "
                               + tr( "from" ).toUtf8().data()
                               + " "
                               + "<img src='qrcx://localhost/flags/" + Country::englishNametoIso2( country ).toUtf8().data()
                               + ".png'/> "
                               + Html::escape( country.toUtf8().data() )
                               + ")</span>"
                               + votes
                               + "</td>";
                articleBody += "</tr>";
              }
            }

            articleBody += "</table>";

            Mutex::Lock _( dataMutex );

            size_t prevSize = data.size();
            
            data.resize( prevSize + articleBody.size() );
  
            memcpy( &data.front() + prevSize, articleBody.data(), articleBody.size() );
  
            hasAnyData = true;

            updated = true;
          }
        }

        QDomNode errors = dd.namedItem( "errors" );

        if ( !errors.isNull() )
          setErrorString( errors.namedItem( "error" ).toElement().text() );
      }
      printf( "done.\n" );
    }
    else
      setErrorString( netReply->errorString() );
  }

  if ( netReplies.empty() )
    finish();
  else
  if ( updated )
    update();
}

sptr< DataRequest > ForvoDictionary::getArticle( wstring const & word,
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
    return new ForvoArticleRequest( word, alts, apiKey, languageCode, getId(),
                                    netMgr );
}

QIcon ForvoDictionary::getIcon() throw()
{
// Experimental code to generate icon -- but the flags clutter the interface too
// much and we're better with a single icon.
#if 0
  if ( languageCode.size() == 2 )
  {
    QString countryCode = Language::countryCodeForId( LangCoder::code2toInt( languageCode.toAscii().data() ) );

    if ( countryCode.size() )
    {
      QImage flag( QString( ":/flags/%1.png" ).arg( countryCode.toLower() ) );

      if ( !flag.isNull() )
      {
        QImage img( ":/icons/forvo_icon_base.png" );

        {
          QPainter painter( &img );
          painter.drawImage( QPoint( 5, 7 ), flag );
        }

        return QIcon( QPixmap::fromImage( img ) );
      }
    }
  }
#endif
  return QIcon( ":/icons/forvo.png" );
}

}

vector< sptr< Dictionary::Class > > makeDictionaries(
                                      Dictionary::Initializing &,
                                      Config::Forvo const & forvo,
                                      QNetworkAccessManager & mgr )
  throw( std::exception )
{
  vector< sptr< Dictionary::Class > > result;

  if ( forvo.enable )
  {
    QStringList codes = forvo.languageCodes.split( ',', QString::SkipEmptyParts );

    QSet< QString > usedCodes;

    for( int x = 0; x < codes.size(); ++x )
    {
      QString code = codes[ x ].simplified();

      if ( code.size() && !usedCodes.contains( code ) )
      {
        // Generate id

        QCryptographicHash hash( QCryptographicHash::Md5 );

        hash.addData( "Forvo source version 1.0" );
        hash.addData( code.toUtf8() );

        QString displayedCode( code.toLower() );

        if ( displayedCode.size() )
          displayedCode[ 0 ] = displayedCode[ 0 ].toUpper();

        result.push_back(
            new ForvoDictionary( hash.result().toHex().data(),
                                 QString( "Forvo (%1)" ).arg( displayedCode ).toUtf8().data(),
                                 forvo.apiKey, code, mgr ) );

        usedCodes.insert( code );
      }
    }
  }

  return result;
}

}