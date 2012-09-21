#include "File.h"
#include "dxcpp/dxcpp.h"

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

namespace fs = boost::filesystem;

#include "api_helper.h"
#include "log.h"

using namespace std;

string File::createResumeInfoString(const int64_t fileSize, const int64_t modifiedTimestamp, const bool toCompress, const int64_t chunkSize, const string &name) {
  using namespace boost;
  string toReturn;
  toReturn += lexical_cast<string>(fileSize) + " ";
  toReturn += lexical_cast<string>(modifiedTimestamp) + " ";
  toReturn += lexical_cast<string>(toCompress) + " ";
  toReturn += lexical_cast<string>(chunkSize) + " ";
  toReturn += name;
  return toReturn;
}

void testLocalFileExists(const string &filename) {
  LOG << "Testing existence of local file " << filename << "...";
  fs::path p(filename);
  if (fs::exists(p)) {
    LOG << " success." << endl;
  } else {
    LOG << " failure." << endl;
    throw runtime_error("Local file " + filename + " does not exist.");
  }
}

int numberOfCompletedParts(const dx::JSON &parts) {
  int64_t numParts = 0;
  for (dx::JSON::const_object_iterator it = parts.object_begin(); it != parts.object_end(); ++it) {
    if (it->second["state"].get<string>() == "complete") {
      numParts++;
    }
  }
  return numParts;
}

/* Returns percentage of the file already uploaded 
 */
double percentageComplete(const dx::JSON &parts, const int64_t size, const int64_t chunkSize) {
  if (size == 0) {
    return 100.0;
  }
  const int completed = numberOfCompletedParts(parts);
  const int lastPartIndex = ((size % chunkSize) == 0) ? int(size / chunkSize) : int(ceil(double(size) / chunkSize));
  bool lastPartDone = false;
  if (parts.has(boost::lexical_cast<string>(lastPartIndex)) && parts[boost::lexical_cast<string>(lastPartIndex)]["state"].get<string>() == "complete") {
    lastPartDone = true;
  }
  const int64_t lastPartSize = size % chunkSize;
  const int64_t totalBytesUploaded = (lastPartDone) ? (((completed - 1) * chunkSize) + lastPartSize) :  (completed * chunkSize);
  return (double(totalBytesUploaded) / size) * 100.0;
}

File::File(const string &localFile_, const string &projectSpec_, const string &folder_, const string &name_, const bool toCompress_, const bool tryResuming, const string &mimeType_, const int64_t chunkSize_, const unsigned fileIndex_)
  : localFile(localFile_), projectSpec(projectSpec_), folder(folder_), name(name_), failed(false), waitOnClose(false), closed(false), toCompress(toCompress_), mimeType(mimeType_), chunkSize(chunkSize_), 
  bytesUploaded(0), fileIndex(fileIndex_) {
  init(tryResuming);
}

void File::init(const bool tryResuming) {
//  bytesUploadedMutex = new boost::mutex();
  projectID = resolveProject(projectSpec);
//  testProjectPermissions(projectID);
  createFolder(projectID, folder);

  testLocalFileExists(localFile);
  
  string remoteFileName = name;
  if (toCompress) 
    remoteFileName += ".gz";
  
  fs::path p(localFile);
  size = fs::file_size(p);
  const int64_t modifiedTimestamp = static_cast<int64_t>(fs::last_write_time(p));
  dx::JSON properties(dx::JSON_OBJECT);
  // Add property {FILE_SIGNATURE_PROPERTY: "<size> <modified time stamp> <toCompress> <chunkSize> <name of file>"
  properties[FILE_SIGNATURE_PROPERTY] = File::createResumeInfoString(size, modifiedTimestamp, toCompress, chunkSize, p.filename().string());
  
  dx::JSON findResult;
  if (tryResuming) {
    // Now check if a resumable file already exist in the project
    findResult = findResumableFileObject(projectID, properties[FILE_SIGNATURE_PROPERTY].get<string>());
    if (findResult.size() == 1) {
      fileID = findResult[0]["id"].get<string>();
      double completePercentage;
      string state = findResult[0]["describe"]["state"].get<string>();
      if (state == "closing" || state == "closed") {
        isRemoteFileOpen = false;
        bytesUploaded = size;
        completePercentage = 100.0;
      } else {
        completePercentage = percentageComplete(findResult[0]["describe"]["parts"], size, chunkSize);
        isRemoteFileOpen = true;
      }

      cerr << "Signature of file " << localFile << " matches remote file: " << findResult[0]["describe"]["name"].get<string>() 
           << " (" << fileID << "), which is " << completePercentage << "% complete ... will resume uploading to it" << endl;
      LOG << "Remote resume target is in state: \"" << state << "\"" << endl;
    }
    if (findResult.size() > 1) {
      cerr << "More than one resumable targets for local file \"" << localFile << "\" found: " << endl;
      for (unsigned i = 0; i < findResult.size(); ++i) {
        cerr << "\t" << (i + 1) << ". " << findResult[i]["describe"]["name"].get<string>() << " (" << findResult[i]["id"].get<string>() << ")" << endl;
      }
      cerr << "Won't upload: \"" << localFile << "\""
           << "Please try cleaning up resumable targets listed above, or run upload agent with --do-not-resume option" << endl;
      failed = true;
    }
  }
  if (!tryResuming || (findResult.size() == 0)) {
    fileID = createFileObject(projectID, folder, remoteFileName, mimeType, properties);
    isRemoteFileOpen = true;
    LOG << "fileID is " << fileID << endl;

    cerr << "Uploading file " << localFile << " to file object " << fileID << endl;
  }
}

unsigned int File::createChunks(BlockingQueue<Chunk *> &queue, const int tries) {
  if (failed || (!isRemoteFileOpen)) {
    // This is the case when:
    // 1. Multiple resumable targets exist for a file (an do-not-resume is not set).
    // 2. OR, Remote resumable target is already in "closing" or "closed" state.
    return 0;
  }
  const dx::JSON desc = fileDescribe(fileID);
  // sanity check
  assert(desc["state"].get<string>() == "open");
  LOG << "Creating chunks:" << endl;
  fs::path p(localFile);
  unsigned int numChunks = 0; // to iterate over chunks
  unsigned int actualChunksCreated = 0; // won't be incremented for case when a chunk is already "complete" while resuming

  for (int64_t start = 0; start < size; start += chunkSize) {
    string partIndex = boost::lexical_cast<string>(numChunks + 1); // minimum part index is 1
    const int64_t end = min(start + chunkSize, size);
    if (desc["parts"].has(partIndex) && desc["parts"][partIndex]["state"] == "complete") {
      LOG << "Part index " << partIndex << " for fileID " << fileID << " is in complete state. Will not create an upload chunk for it." << endl;
      bytesUploaded += (end - start);
      // TODO :Should we assert here for part size (for sanity check). What to do
      //       if it fails ?
    } else { 
      const bool lastChunk = ((start + chunkSize) >= size);
      Chunk * c = new Chunk(localFile, fileID, numChunks, tries, start, end, toCompress, lastChunk, fileIndex);
      c->log("created");
      queue.produce(c);
      actualChunksCreated++;
    }
    ++numChunks;
  }
  return actualChunksCreated++;
}

void File::close(void) {
  closeFileObject(fileID);
}

void File::updateState(void) {
  string state = getFileState(fileID);
  if (state == "closed") {
    LOG << "File " << fileID << " is closed." << endl;
  }
  closed = (state == "closed");
}

ostream &operator<<(ostream &out, const File &file) {
  out << file.localFile << " (" << file.fileID << ")";
  return out;
}
