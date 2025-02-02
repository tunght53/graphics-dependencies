//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

//
// WASAPICapture.h
//

#include "pch.h"
#include "WASAPICapture.h"

using namespace Windows::Storage;
using namespace Windows::System::Threading;
using namespace SDKSample::WASAPIAudio;


//
//  WASAPICapture()
//
WASAPICapture::WASAPICapture() :
    m_BufferFrames( 0 ),
    m_cbDataSize( 0 ),
    m_cbHeaderSize( 0 ),
    m_cbFlushCounter( 0 ),
    m_dwQueueID( 0 ),
    m_DeviceStateChanged( nullptr ),
    m_AudioClient( nullptr ),
    m_AudioCaptureClient( nullptr ),
    m_SampleReadyAsyncResult( nullptr ),
    m_ContentStream( nullptr ),
    m_OutputStream( nullptr ),
    //m_WAVDataWriter( nullptr ),
    //m_PlotData( nullptr ),
    m_fWriting( false ),
    m_sampleReceivedUserCallback( nullptr ),
    m_userData( nullptr )
{
    // Create events for sample ready or user stop
    m_SampleReadyEvent = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
    m_EventHandle = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    if (nullptr == m_SampleReadyEvent)
    {
        ThrowIfFailed( HRESULT_FROM_WIN32( GetLastError() ) );
    }

    if (!InitializeCriticalSectionEx( &m_CritSec, 0, 0 ))
    {
        ThrowIfFailed( HRESULT_FROM_WIN32( GetLastError() ) );
    }

    m_DeviceStateChanged = ref new DeviceStateChangedEvent();
    if (nullptr == m_DeviceStateChanged)
    {
        ThrowIfFailed( E_OUTOFMEMORY );
    }

    // Register MMCSS work queue
    HRESULT hr = S_OK;
    DWORD dwTaskID = 0;

    hr = MFLockSharedWorkQueue( L"Capture", 0, &dwTaskID, &m_dwQueueID );
    if (FAILED( hr ))
    {
        ThrowIfFailed( hr );
    }

    // Set the capture event work queue to use the MMCSS queue
    m_xSampleReady.SetQueueID( m_dwQueueID );
}

//
//  ~WASAPICapture()
//
WASAPICapture::~WASAPICapture()
{
    SAFE_RELEASE( m_AudioClient );
    SAFE_RELEASE( m_AudioCaptureClient );
    SAFE_RELEASE( m_SampleReadyAsyncResult );

    if (INVALID_HANDLE_VALUE != m_SampleReadyEvent)
    {
        CloseHandle( m_SampleReadyEvent );
        m_SampleReadyEvent = INVALID_HANDLE_VALUE;
    }
    if (INVALID_HANDLE_VALUE != m_EventHandle)
    {
      CloseHandle(m_EventHandle);
      m_EventHandle = INVALID_HANDLE_VALUE;
    }

    MFUnlockWorkQueue( m_dwQueueID );

    m_DeviceStateChanged = nullptr;
    m_ContentStream = nullptr;
    m_OutputStream = nullptr;
    //m_WAVDataWriter = nullptr;

    //m_PlotData = nullptr;

    DeleteCriticalSection( &m_CritSec );
}

//
//  InitializeAudioDeviceAsync()
//
//  Activates the default audio capture on a asynchronous callback thread.  This needs
//  to be called from the main UI thread.
//
HRESULT WASAPICapture::InitializeAudioDeviceAsync(SampleReceivedUserCallback sampleReceivedUserCallback, void *userData)
{
    IActivateAudioInterfaceAsyncOperation *asyncOp;
    HRESULT hr = S_OK;

    m_sampleReceivedUserCallback = sampleReceivedUserCallback;
    m_userData = userData;

    // Get a string representing the Default Audio Capture Device
    m_DeviceIdString = MediaDevice::GetDefaultAudioCaptureId( Windows::Media::Devices::AudioDeviceRole::Default );

    // This call must be made on the main UI thread.  Async operation will call back to 
    // IActivateAudioInterfaceCompletionHandler::ActivateCompleted, which must be an agile interface implementation
    hr = ActivateAudioInterfaceAsync( m_DeviceIdString->Data(), __uuidof(IAudioClient2), nullptr, this, &asyncOp );
    if (FAILED( hr ))
    {
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, hr, true );
    }

    SAFE_RELEASE( asyncOp );
    return hr;
}

//
//  ActivateCompleted()
//
//  Callback implementation of ActivateAudioInterfaceAsync function.  This will be called on MTA thread
//  when results of the activation are available.
//
HRESULT WASAPICapture::ActivateCompleted( IActivateAudioInterfaceAsyncOperation *operation )
{
    HRESULT hr = S_OK;
    HRESULT hrActivateResult = S_OK;
    IUnknown *punkAudioInterface = nullptr;

    // Check for a successful activation result
    hr = operation->GetActivateResult( &hrActivateResult, &punkAudioInterface );
    if (SUCCEEDED( hr ) && SUCCEEDED( hrActivateResult ))
    {
        // Get the pointer for the Audio Client
        punkAudioInterface->QueryInterface( IID_PPV_ARGS(&m_AudioClient) );
        if (nullptr == m_AudioClient)
        {
            hr = E_FAIL;
            goto exit;
        }

        hr = m_AudioClient->GetMixFormat( &m_MixFormat );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // convert from Float to PCM and from WAVEFORMATEXTENSIBLE to WAVEFORMATEX
        if ( (m_MixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
             ( (m_MixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) &&
               (reinterpret_cast<WAVEFORMATEXTENSIBLE *>(m_MixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ) )
        {
            m_MixFormat->wFormatTag = WAVE_FORMAT_PCM;
            m_MixFormat->wBitsPerSample = 16;
            m_MixFormat->nBlockAlign = m_MixFormat->nChannels * 2;    // (nChannels * wBitsPerSample) / 8
            m_MixFormat->nAvgBytesPerSec = m_MixFormat->nSamplesPerSec * m_MixFormat->nBlockAlign;
            m_MixFormat->cbSize = 0;
        }

        // Initialize the AudioClient in Shared Mode with the user specified buffer
        hr = m_AudioClient->Initialize( AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        200000,
                                        0,
                                        m_MixFormat,
                                        nullptr );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // Get the maximum size of the AudioClient Buffer
        hr = m_AudioClient->GetBufferSize( &m_BufferFrames );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // Get the capture client
        hr = m_AudioClient->GetService( __uuidof(IAudioCaptureClient), (void**) &m_AudioCaptureClient );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // Create Async callback for sample events
        hr = MFCreateAsyncResult( nullptr, &m_xSampleReady, nullptr, &m_SampleReadyAsyncResult );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // Sets the event handle that the system signals when an audio buffer is ready to be processed by the client
        hr = m_AudioClient->SetEventHandle( m_SampleReadyEvent );
        if (FAILED( hr ))
        {
            goto exit;
        }

        // Create the visualization array
        //hr = InitializeScopeData();
        //if (FAILED( hr ))
        //{
        //    goto exit;
        //}

        // Creates the WAV file.  If successful, will set the Initialized event
        //hr = CreateWAVFile();
        //if (FAILED( hr ))
        //{
        //    goto exit;
        //}
        try
        {
            m_DeviceStateChanged->SetState( DeviceState::DeviceStateInitialized, S_OK, true );
        }
        catch (Platform::Exception ^e)
        {
            m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, e->HResult, true );
        }
    }

exit:
    SAFE_RELEASE( punkAudioInterface );

    if (FAILED( hr ))
    {
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, hr, true );
        SAFE_RELEASE( m_AudioClient );
        SAFE_RELEASE( m_AudioCaptureClient );
        SAFE_RELEASE( m_SampleReadyAsyncResult );
    }

    SetEvent(m_EventHandle);
    
    // Need to return S_OK
    return S_OK;
}

//
//  CreateWAVFile()
//
//  Creates a WAV file in KnownFolders::MusicLibrary
//
//HRESULT WASAPICapture::CreateWAVFile()
//{
//    // Create the WAV file, appending a number if file already exists
//    concurrency::task<StorageFile^>( KnownFolders::MusicLibrary->CreateFileAsync( AUDIO_FILE_NAME, CreationCollisionOption::GenerateUniqueName )).then(
//        [this]( StorageFile^ file )
//    {
//        if (nullptr == file)
//        {
//            ThrowIfFailed( E_INVALIDARG );
//        }
//
//        return file->OpenAsync( FileAccessMode::ReadWrite );
//    })
//
//    // Then create a RandomAccessStream
//    .then([this]( IRandomAccessStream^ stream )
//    {
//        if (nullptr == stream)
//        {
//            ThrowIfFailed( E_INVALIDARG );
//        }
//
//        // Get the OutputStream for the file
//        m_ContentStream = stream;
//        m_OutputStream = m_ContentStream->GetOutputStreamAt(0);
//        
//        // Create the DataWriter
//        m_WAVDataWriter = ref new DataWriter( m_OutputStream );
//        if (nullptr == m_WAVDataWriter)
//        {
//            ThrowIfFailed( E_OUTOFMEMORY );
//        }
//
//        // Create the WAV header
//        DWORD header[] = {
//            FCC('RIFF'),        // RIFF header
//            0,                  // Total size of WAV (will be filled in later)
//            FCC('WAVE'),        // WAVE FourCC
//            FCC('fmt '),        // Start of 'fmt ' chunk
//            sizeof(WAVEFORMATEX) + m_MixFormat->cbSize      // Size of fmt chunk
//        };
//
//        DWORD data[] = { FCC('data'), 0 };  // Start of 'data' chunk
//
//        auto headerBytes = ref new Platform::Array<BYTE>( reinterpret_cast<BYTE*>(header), sizeof(header) );
//        auto formatBytes = ref new Platform::Array<BYTE>( reinterpret_cast<BYTE*>(m_MixFormat), sizeof(WAVEFORMATEX) );
//        auto dataBytes = ref new Platform::Array<BYTE>( reinterpret_cast<BYTE*>(data), sizeof(data) );
//
//        if ( (nullptr == headerBytes) || (nullptr == formatBytes) || (nullptr == dataBytes) )
//        {
//            ThrowIfFailed( E_OUTOFMEMORY );
//        }
//
//        // Write the header
//        m_WAVDataWriter->WriteBytes( headerBytes );
//        m_WAVDataWriter->WriteBytes( formatBytes );
//        m_WAVDataWriter->WriteBytes( dataBytes );
//
//        return m_WAVDataWriter->StoreAsync();
//    })
//
//    // Wait for file data to be written to file
//    .then([this]( unsigned int BytesWritten )
//    {
//        m_cbHeaderSize = BytesWritten;
//        return m_WAVDataWriter->FlushAsync();
//    })
//
//    // Our file is ready to go, so we can now signal that initialziation is finished
//    .then([this]( bool f )
//    {
//        try
//        {
//            m_DeviceStateChanged->SetState( DeviceState::DeviceStateInitialized, S_OK, true );
//        }
//        catch (Platform::Exception ^e)
//        {
//            m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, e->HResult, true );
//        }
//    });
//
//    return S_OK;
//}

//
//  FixWAVHeader()
//
//  The size values were not known when we originally wrote the header, so now go through and fix the values
//
//HRESULT WASAPICapture::FixWAVHeader()
//{
//    auto DataSizeByte = ref new Platform::Array<BYTE>( reinterpret_cast<BYTE*>( &m_cbDataSize ), sizeof(DWORD) );
//    
//    // Write the size of the 'data' chunk first
//    IOutputStream^ OutputStream = m_ContentStream->GetOutputStreamAt( m_cbHeaderSize - sizeof(DWORD) );
//    m_WAVDataWriter = ref new DataWriter( OutputStream );
//    m_WAVDataWriter->WriteBytes( DataSizeByte );
//
//    concurrency::task<unsigned int>( m_WAVDataWriter->StoreAsync()).then(
//        [this]( unsigned int BytesWritten )
//    {
//        DWORD cbTotalSize = m_cbDataSize + m_cbHeaderSize - 8;
//        auto TotalSizeByte = ref new Platform::Array<BYTE>( reinterpret_cast<BYTE*>( &cbTotalSize ), sizeof(DWORD) );
//
//        // Write the total file size, minus RIFF chunk and size
//        IOutputStream^ OutputStream = m_ContentStream->GetOutputStreamAt( sizeof(DWORD) );  // sizeof(DWORD) == sizeof(FOURCC)
//        m_WAVDataWriter = ref new DataWriter( OutputStream );
//        m_WAVDataWriter->WriteBytes( TotalSizeByte );
//
//        concurrency::task<unsigned int>( m_WAVDataWriter->StoreAsync()).then(
//            [this]( unsigned int BytesWritten )
//        {
//            return m_WAVDataWriter->FlushAsync();
//        })
//        
//        .then(
//            [this]( bool f )
//        {
//            m_DeviceStateChanged->SetState( DeviceState::DeviceStateStopped, S_OK, true );
//        });
//    });
//
//    return S_OK;
//}

//
//  InitializeScopeData()
//
//  Setup data structures for sample visualizations
//
//HRESULT WASAPICapture::InitializeScopeData()
//{
//    HRESULT hr = S_OK;
//
//    m_cPlotDataFilled = 0;
//    m_cPlotDataMax = (MILLISECONDS_TO_VISUALIZE * m_MixFormat->nSamplesPerSec) / 1000;
//
//    m_PlotData = ref new Platform::Array<int, 1>( m_cPlotDataMax + 1 );
//    if (nullptr == m_PlotData)
//    {
//        return E_OUTOFMEMORY;
//    }
//
//    // Only Support 16 bit Audio for now
//    if (m_MixFormat->wBitsPerSample == 16)
//    {
//        m_PlotData[ m_cPlotDataMax ] = -32768;  // INT16_MAX
//    }
//    else
//    {
//        m_PlotData = nullptr;
//        hr = S_FALSE;
//    }
//
//    return hr;
//}

//
//  StartCaptureAsync()
//
//  Starts asynchronous capture on a separate thread via MF Work Item
//
HRESULT WASAPICapture::StartCaptureAsync()
{
    HRESULT hr = S_OK;

    // We should be in the initialzied state if this is the first time through getting ready to capture.
    //if (m_DeviceStateChanged->GetState() == DeviceState::DeviceStateInitialized)
    concurrency::create_task([this]()
    {
        WaitForSingleObjectEx(m_EventHandle, INFINITE, FALSE);
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateStarting, S_OK, true );
        MFPutWorkItem2( MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStartCapture, nullptr );
    });
    return hr;

    // We are in the wrong state
    //return E_NOT_VALID_STATE;
}

//
//  OnStartCapture()
//
//  Callback method to start capture
//
HRESULT WASAPICapture::OnStartCapture( IMFAsyncResult* pResult )
{
    HRESULT hr = S_OK;

    // Start the capture
    hr = m_AudioClient->Start();
    if (SUCCEEDED( hr ))
    {
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateCapturing, S_OK, true );
        MFPutWaitingWorkItem( m_SampleReadyEvent, 0, m_SampleReadyAsyncResult, &m_SampleReadyKey );
    }
    else
    {
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, hr, true );
    }

    return S_OK;
}

//
//  StopCaptureAsync()
//
//  Stop capture asynchronously via MF Work Item
//
HRESULT WASAPICapture::StopCaptureAsync()
{
    if ( (m_DeviceStateChanged->GetState() != DeviceState::DeviceStateCapturing) &&
         (m_DeviceStateChanged->GetState() != DeviceState::DeviceStateInError) )
    {
        return E_NOT_VALID_STATE;
    }

    m_DeviceStateChanged->SetState( DeviceState::DeviceStateStopping, S_OK, true );

    return MFPutWorkItem2( MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStopCapture, nullptr );
}

//
//  OnStopCapture()
//
//  Callback method to stop capture
//
HRESULT WASAPICapture::OnStopCapture( IMFAsyncResult* pResult )
{
    // Stop capture by cancelling Work Item
    // Cancel the queued work item (if any)
    if (0 != m_SampleReadyKey)
    {
        MFCancelWorkItem( m_SampleReadyKey );
        m_SampleReadyKey = 0;
    }

    m_AudioClient->Stop();
    SAFE_RELEASE( m_SampleReadyAsyncResult );

    // If this is set, it means we writing from the memory buffer to the actual file asynchronously
    // Since a second call to StoreAsync() can cause an exception, don't queue this now, but rather
    // let the async operation completion handle the call.
    //if (!m_fWriting)
    //{
    //    m_DeviceStateChanged->SetState( DeviceState::DeviceStateFlushing, S_OK, true );

    //    concurrency::task<unsigned int>( m_WAVDataWriter->StoreAsync()).then(
    //        [this]( unsigned int BytesWritten )
    //    {
    //        FinishCaptureAsync();
    //    });
    //}

    return S_OK;
}

//
//  FinishCaptureAsync()
//
//  Finalizes WAV file on a separate thread via MF Work Item
//
HRESULT WASAPICapture::FinishCaptureAsync()
{
    // We should be flushing when this is called
    if (m_DeviceStateChanged->GetState() == DeviceState::DeviceStateFlushing)
    {
        return MFPutWorkItem2( MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xFinishCapture, nullptr ); 
    }

    // We are in the wrong state
    return E_NOT_VALID_STATE;
}

//
//  OnFinishCapture()
//
//  Because of the asynchronous nature of the MF Work Queues and the DataWriter, there could still be
//  a sample processing.  So this will get called to finalize the WAV header.
//
HRESULT WASAPICapture::OnFinishCapture( IMFAsyncResult* pResult )
{
    // FixWAVHeader will set the DeviceStateStopped when all async tasks are complete
    //return FixWAVHeader();
    return S_OK;
}

//
//  OnSampleReady()
//
//  Callback method when ready to fill sample buffer
//
HRESULT WASAPICapture::OnSampleReady( IMFAsyncResult* pResult )
{
    HRESULT hr = S_OK;

    hr = OnAudioSampleRequested( false );

    if (SUCCEEDED( hr ))
    {
        // Re-queue work item for next sample
        if (m_DeviceStateChanged->GetState() ==  DeviceState::DeviceStateCapturing)
        {
            hr = MFPutWaitingWorkItem( m_SampleReadyEvent, 0, m_SampleReadyAsyncResult, &m_SampleReadyKey );
        }
    }
    else
    {
        m_DeviceStateChanged->SetState( DeviceState::DeviceStateInError, hr, true );
    }
    
    return hr;
}

//
//  OnAudioSampleRequested()
//
//  Called when audio device fires m_SampleReadyEvent
//
HRESULT WASAPICapture::OnAudioSampleRequested( Platform::Boolean IsSilence )
{
    HRESULT hr = S_OK;
    UINT32 FramesAvailable = 0;
    BYTE *Data = nullptr;
    DWORD dwCaptureFlags;
    UINT64 u64DevicePosition = 0;
    UINT64 u64QPCPosition = 0;
    DWORD cbBytesToCapture = 0;

    EnterCriticalSection( &m_CritSec );

    // If this flag is set, we have already queued up the async call to finialize the WAV header
    // So we don't want to grab or write any more data that would possibly give us an invalid size
    if ( (m_DeviceStateChanged->GetState() == DeviceState::DeviceStateStopping) ||
         (m_DeviceStateChanged->GetState() == DeviceState::DeviceStateFlushing) )
    {
        goto exit;
    }

    // This should equal the buffer size when GetBuffer() is called
    hr = m_AudioCaptureClient->GetNextPacketSize( &FramesAvailable );
    if (FAILED( hr ))
    {
        goto exit;
    }

    if (FramesAvailable > 0)
    {
        cbBytesToCapture = FramesAvailable * m_MixFormat->nBlockAlign;
        
        // WAV files have a 4GB (0xFFFFFFFF) size limit, so likely we have hit that limit when we
        // overflow here.  Time to stop the capture
        if ( (m_cbDataSize + cbBytesToCapture) < m_cbDataSize )
        {
            StopCaptureAsync();
            goto exit;
        }

        // Get sample buffer
        hr = m_AudioCaptureClient->GetBuffer( &Data, &FramesAvailable, &dwCaptureFlags, &u64DevicePosition, &u64QPCPosition );
        if (FAILED( hr ))
        {
            goto exit;
        }

        if (dwCaptureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        {
            // Pass down a discontinuity flag in case the app is interested and reset back to capturing
            m_DeviceStateChanged->SetState( DeviceState::DeviceStateDiscontinuity, S_OK, true );
            m_DeviceStateChanged->SetState( DeviceState::DeviceStateCapturing, S_OK, false );
        }

        // Zero out sample if silence
        if ( (dwCaptureFlags & AUDCLNT_BUFFERFLAGS_SILENT) || IsSilence )
        {
            memset( Data, 0, FramesAvailable * m_MixFormat->nBlockAlign );
        }

        m_sampleReceivedUserCallback(m_userData, Data, FramesAvailable, cbBytesToCapture, true);

        // Store data in array
        //auto dataByte = ref new Platform::Array<BYTE, 1>( Data, cbBytesToCapture );

        // Release buffer back
        m_AudioCaptureClient->ReleaseBuffer( FramesAvailable );

        // Update plotter data
        //ProcessScopeData( Data, cbBytesToCapture );

        // Write File and async store
        //m_WAVDataWriter->WriteBytes( dataByte );

        // Increase the size of our 'data' chunk and flush counter.  m_cbDataSize needs to be accurate
        // Its OK if m_cbFlushCounter is an approximation
        m_cbDataSize += cbBytesToCapture;
        m_cbFlushCounter += cbBytesToCapture;

        //if ( (m_cbFlushCounter > ( m_MixFormat->nAvgBytesPerSec * FLUSH_INTERVAL_SEC )) && !m_fWriting )
        //{
        //    // Set this flag when about to commit the async storage operation.  We don't want to 
        //    // accidently call stop and finalize the WAV header or run into a scenario where the 
        //    // store operation takes longer than FLUSH_INTERVAL_SEC as multiple concurrent calls 
        //    // to StoreAsync() can cause an exception
        //    m_fWriting = true;

        //    // Reset the counter now since we can process more samples during the async callback
        //    m_cbFlushCounter = 0;

        //    concurrency::task<unsigned int>( m_WAVDataWriter->StoreAsync()).then(
        //        [this]( unsigned int BytesWritten )
        //    {
        //        m_fWriting = false;

        //        // We need to check for StopCapture while we are flusing the file.  If it has come through, then we
        //        // can go ahead and call FinisheCaptureAsync() to write the WAV header
        //        if (m_DeviceStateChanged->GetState() == DeviceState::DeviceStateStopping)
        //        {
        //            m_DeviceStateChanged->SetState( DeviceState::DeviceStateFlushing, S_OK, true );
        //            FinishCaptureAsync();
        //        }
        //    });
        //}
    }

exit:
    LeaveCriticalSection( &m_CritSec );

    return hr;
}

//
//  ProcessScopeData()
//
//  Copies sample data to the buffer array and fires the event
//
//HRESULT WASAPICapture::ProcessScopeData( BYTE* pData, DWORD cbBytes )
//{
//    HRESULT hr = S_OK;
//
//    // We don't have a valid pointer array, so return.  This could be the case if we aren't
//    // dealing with 16-bit audio
//    if (m_PlotData == nullptr)
//    {
//        return S_FALSE;
//    }
//
//    DWORD dwNumPoints = cbBytes / m_MixFormat->nChannels / (m_MixFormat->wBitsPerSample / 8);
//
//    // Read the 16-bit samples from channel 0
//    INT16 *pi16 = (INT16*)pData;
//
//    for ( DWORD i = 0; m_cPlotDataFilled < m_cPlotDataMax && i < dwNumPoints; i++ )
//    {
//        m_PlotData[ m_cPlotDataFilled ] = *pi16;
//        pi16 += m_MixFormat->nChannels;
//
//        m_cPlotDataFilled++;
//    }
//
//    // Send off the event and get ready for the next set of samples
//    if (m_cPlotDataFilled == m_cPlotDataMax)
//    {
//        ComPtr<IUnknown> spUnknown;
//        ComPtr<CAsyncState> spState = Make<CAsyncState>( m_PlotData, m_cPlotDataMax + 1 );
//
//        hr = spState.As( &spUnknown );
//        if (SUCCEEDED( hr ))
//        {
//            MFPutWorkItem2( MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xSendScopeData, spUnknown.Get() );
//        }
//
//        m_cPlotDataFilled = 0;
//    }
//
//    return hr;
//}

//
//  OnSendScopeData()
//
//  Callback method to stop capture
//
//HRESULT WASAPICapture::OnSendScopeData( IMFAsyncResult* pResult )
//{
//    HRESULT hr = S_OK;
//    CAsyncState *pState = nullptr;
//    
//    hr = pResult->GetState( reinterpret_cast<IUnknown**>(&pState) );
//    if (SUCCEEDED( hr ))
//    {
//        PlotDataReadyEvent::SendEvent( reinterpret_cast<Platform::Object^>(this), pState->m_Data , pState->m_Size );
//    }
//
//    SAFE_RELEASE( pState );
//
//    return S_OK;
//}

