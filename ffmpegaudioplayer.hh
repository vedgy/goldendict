/* This file is (c) 2018 Igor Kushnir <igorkuo@gmail.com>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef FFMPEGAUDIOPLAYER_HH_INCLUDED
#define FFMPEGAUDIOPLAYER_HH_INCLUDED

#include "audioplayerinterface.hh"
#include "ffmpegaudio.hh"

#ifdef MAKE_FFMPEG_PLAYER

namespace Ffmpeg
{

class AudioPlayer : public AudioPlayerInterface
{
  Q_OBJECT
public:
  AudioPlayer()
  {
    connect( &AudioService::instance(), SIGNAL( error( QString ) ),
             this, SIGNAL( error( QString ) ) );
    connect( &AudioService::instance(), SIGNAL( playbackStopped() ),
             this, SLOT( onPlaybackStopped() ) );
  }

  ~AudioPlayer()
  {
    stop();
  }

  virtual QString play( const char * data, int size )
  {
    AudioService::instance().playMemory( data, size );
    emit stateChanged( PlayingState );
    return QString();
  }

  virtual void stop()
  {
    AudioService::instance().stop();
    emit stateChanged( StoppedState );
  }

private slots:
  void onPlaybackStopped()
  {
    emit stateChanged( StoppedState );
  }
};

}

#endif // MAKE_FFMPEG_PLAYER

#endif // FFMPEGAUDIOPLAYER_HH_INCLUDED
