/*
 * qedit-defs.h
 *
 * The Sample Grabber interfaces (ISampleGrabber / ISampleGrabberCB) used to
 * be declared in <qedit.h> of the legacy DirectX SDK. Modern Windows SDKs
 * (and MinGW-w64) no longer ship qedit.h, so the interfaces and their GUIDs
 * are declared here manually. The implementation itself (qedit.dll) is still
 * shipped with every copy of Windows.
 *
 * This is the same approach used by FFmpeg, VLC and many other DirectShow
 * based projects.
 */

#pragma once

#include <dshow.h>
#include <unknwn.h>

// {6B652FFF-11FE-4fce-92AD-0266B5D7C78F}
static const CLSID CLSID_SampleGrabber = {
    0x6B652FFF,
    0x11FE,
    0x4fce,
    {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};

// {C1F400A4-3F08-11d3-9F0B-006008039E37}
static const CLSID CLSID_NullRenderer = {
    0xC1F400A4,
    0x3F08,
    0x11d3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

// {C1F400A0-3F08-11d3-9F0B-006008039E37}
static const IID IID_ISampleGrabber = {
    0xC1F400A0,
    0x3F08,
    0x11d3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

// {0579154A-2B53-4994-B0D0-E773148EFF85}
static const IID IID_ISampleGrabberCB = {
    0x0579154A,
    0x2B53,
    0x4994,
    {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

interface ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime,
                                              IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime,
                                               BYTE *pBuffer,
                                               long bufferLen) = 0;
};

interface ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(
        const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(
        AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize,
                                                       long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(
        IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(
        ISampleGrabberCB *pCallback, long whichMethodToCallback) = 0;
};
