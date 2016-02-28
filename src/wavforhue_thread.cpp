/*
*      Copyright (C) 2015-2016 Thomas M. Hardy
*      Copyright (C) 2003-2016 Team Kodi
*      http://kodi.tv
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, see
*  <http://www.gnu.org/licenses/>.
*
*/

	


#ifndef WAVFORHUE_THREAD
#include "WavforHue_Thread.h"
#endif

using namespace ADDON;

// -- trim ---------------------------------------------------------
// trim from start
static inline std::string &ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
  return s;
}

// trim from end
static inline std::string &rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
  return s;
}

// trim from both ends
static inline std::string &trim(std::string &s) {
  return ltrim(rtrim(s));
}
// -- trim ---------------------------------------------------------

// -- Constructor ----------------------------------------------------
WavforHue_Thread::WavforHue_Thread()
{
}
// -- Constructor ----------------------------------------------------

// -- Destructor ----------------------------------------------------- 
WavforHue_Thread::~WavforHue_Thread()
{
}
// -- Destructor ----------------------------------------------------- 

// -- Threading ----------------------------------------------------------
// This thread keeps cURL from puking all over the waveform, suprising it and
// making it jerk away.
void WavforHue_Thread::workerThread()
{
  bool isEmpty;
  SocketData putData;
  std::queue<SocketData> mQueue;
  // This thread comes alive when AudioData(), Create() or Start() has put an 
  // item in the stack. It runs until Destroy() or Stop() sets gRunThread to 
  // false and joins it. Or something like that. It's actually magic.
  while (gRunThread || !mQueue.empty())
  {  
    //check that an item is on the stack
    {
      std::lock_guard<std::mutex> lock(gMutex);
      isEmpty = gQueue.empty();
    }
    if (isEmpty)
    {
      //Wait until AudioData() sends data.
      std::unique_lock<std::mutex> lock(gMutex);
      gThreadConditionVariable.wait(lock, [&]{return gReady; });
    }
    else
    {
      // Get everything off the global queue for local processing
      std::lock_guard<std::mutex> lock(gMutex);
      while (!gQueue.empty())
      {
        mQueue.push(gQueue.front());
        gQueue.pop();
      }
    }
    while (!mQueue.empty())
    {
      putData = mQueue.front(); mQueue.pop();
      httpRequest(putData);
    }
  }
}

void WavforHue_Thread::transferQueueToMain()
{
	SocketData putData;
	while (!wavforhue.queue.empty())
	{
    putData = wavforhue.queue.front(); wavforhue.queue.pop();
    httpRequest(putData);
  }
}

void WavforHue_Thread::transferQueueToThread()
{
  SocketData putData;
  gRunThread = true;
  // Check if the thread is alive yet.
  if (!gWorkerThread.joinable())
  {
    gWorkerThread = std::thread(&WavforHue_Thread::workerThread, this);
  }
  while (!wavforhue.queue.empty())
  {
    putData = wavforhue.queue.front();
    wavforhue.queue.pop();
    //if (gQueue.size() < 10)
    //{
      std::lock_guard<std::mutex> lock(gMutex);
      gQueue.push(putData);
    //}
  }
  // Let the thread know to start processing.
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gReady = true;
  }
  gThreadConditionVariable.notify_one();
}

void WavforHue_Thread::stop()
{
  gRunThread = false;
  while (gWorkerThread.joinable())  // Kill 'em all \m/
  {
    gWorkerThread.join();
  }
}
//-- Threading -----------------------------------------------------

// -- HTTP functions -----------------------------------------------
// This helps cURL store the HTTP response in a string
size_t WavforHue_Thread::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

void WavforHue_Thread::httpRequest(SocketData socketData)
{
#ifndef _WIN32
  CURL *curl;
  CURLcode res;
  std::string url = "http://" + socketData.host + socketData.path;
  curl = curl_easy_init();
  // Now specify we want to PUT data, but not using a file, so it has o be a CUSTOMREQUEST
  curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
  if (url.substr(url.length() - 3) == "api")
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  else
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  // This eliminates all kinds of HTTP responses from showing up in stdin.
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WavforHue_Thread::WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, socketData.json.c_str());
  // Set the URL that is about to receive our POST. 
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // Perform the request, res will get the return code
  res = curl_easy_perform(curl);
  // always cleanup curl
  curl_easy_cleanup(curl);
#else
  std::string request, error;
  char buffer[BUFFERSIZE];

  WSADATA wsaData;
  SOCKET ConnectSocket = INVALID_SOCKET;
  struct addrinfo *result = NULL,
    *ptr = NULL,
    hints;
  int iResult;

  // Initialize Winsock
  iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (iResult != 0) {
    error = "WSAStartup failed with error: " + iResult;
    XBMC->Log(LOG_DEBUG, error.c_str());
    WSACleanup();
    abort();
  }

  ZeroMemory(&hints, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // Resolve the server address and port
  iResult = getaddrinfo(socketData.host.c_str(), socketData.port.c_str(), &hints, &result);
  if (iResult != 0) {
    error = "getaddrinfo failed with error: " + iResult;
    XBMC->Log(LOG_DEBUG, error.c_str());
    WSACleanup();
    abort();
  }

  // Attempt to connect to an address until one succeeds
  for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {

    // Create a SOCKET for connecting to server
    ConnectSocket = socket(ptr->ai_family, ptr->ai_socktype,
      ptr->ai_protocol);
    if (ConnectSocket == INVALID_SOCKET) {
      error = "socket failed with error: " + WSAGetLastError();
      XBMC->Log(LOG_DEBUG, error.c_str());
      WSACleanup();
      abort();
    }

    // Connect to server.
    iResult = connect(ConnectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
      closesocket(ConnectSocket);
      ConnectSocket = INVALID_SOCKET;
      continue;
    }
    break;
  }

  freeaddrinfo(result);

  if (ConnectSocket == INVALID_SOCKET) {
    error = "Unable to connect to server!";
    XBMC->Log(LOG_DEBUG, error.c_str());
    WSACleanup();
    abort();
  }

  // Send an initial buffer
  std::stringstream ss;
  ss << socketData.json.length();

  std::stringstream request2;

  request2 << socketData.method << " " << socketData.path << " HTTP/1.1" << std::endl;
  request2 << "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1; .NET CLR 1.1.4322; .NET CLR 2.0.50727)" << std::endl;
  //request2 << "" << endl;
  request2 << "Host: " << socketData.host << std::endl;
  request2 << "Content-Length: " << socketData.json.length() << std::endl;

  request2 << "Content-Type: application/x-www-form-urlencoded" << std::endl;
  request2 << "Accept-Language: en" << std::endl;
  request2 << std::endl;
  request2 << socketData.json;
  request = request2.str();

  iResult = send(ConnectSocket, request.c_str(), request.length(), 0);
  if (iResult == SOCKET_ERROR) {
    error = "send failed with error: " + WSAGetLastError();
    XBMC->Log(LOG_DEBUG, error.c_str());
    closesocket(ConnectSocket);
    WSACleanup();
    abort();
  }

  //XBMC->Log(LOG_DEBUG, "Connected.");

  // shutdown the connection since no more data will be sent
  iResult = shutdown(ConnectSocket, SD_SEND);
  if (iResult == SOCKET_ERROR) {
    error = "shutdown failed with error: " + WSAGetLastError();
    XBMC->Log(LOG_DEBUG, error.c_str());
    closesocket(ConnectSocket);
    WSACleanup();
    abort();
  }

  // Receive until the peer closes the connection
  response = "";
  do {
    iResult = recv(ConnectSocket, (char*)&buffer, BUFFERSIZE, 0);
    if (iResult > 0)
      response += std::string(buffer).substr(0, iResult);
    else if (iResult == 0)
      //XBMC->Log(LOG_DEBUG, "Connection closed.");
      ((void)0);
    else
    {
      error = "recv failed with error: " + WSAGetLastError();
      XBMC->Log(LOG_DEBUG, error.c_str());
    }
  } while (iResult > 0);

  // cleanup
  closesocket(ConnectSocket);
  WSACleanup();
#endif
  // Response is holding the json response from the Hue bridge;
  response = trim(response.substr(response.find("\r\n\r\n")));
  XBMC->Log(LOG_DEBUG, response.c_str());
}
// -- HTTP functions -----------------------------------------------