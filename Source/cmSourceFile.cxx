/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmSourceFile.h"

#include <sstream>

#include "cmCustomCommand.h"
#include "cmGlobalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmProperty.h"
#include "cmState.h"
#include "cmSystemTools.h"
#include "cmake.h"

cmSourceFile::cmSourceFile(cmMakefile* mf, const std::string& name,
                           cmSourceFileLocationKind kind)
  : Location(mf, name, kind)
{
}

cmSourceFile::~cmSourceFile()
{
  this->SetCustomCommand(nullptr);
}

std::string const& cmSourceFile::GetExtension() const
{
  return this->Extension;
}

const std::string cmSourceFile::propLANGUAGE = "LANGUAGE";
const std::string cmSourceFile::propLOCATION = "LOCATION";
const std::string cmSourceFile::propGENERATED = "GENERATED";

void cmSourceFile::SetObjectLibrary(std::string const& objlib)
{
  this->ObjectLibrary = objlib;
}

std::string cmSourceFile::GetObjectLibrary() const
{
  return this->ObjectLibrary;
}

std::string cmSourceFile::GetLanguage()
{
  // If the language was set explicitly by the user then use it.
  if (const char* lang = this->GetProperty(propLANGUAGE)) {
    return lang;
  }

  // Perform computation needed to get the language if necessary.
  if (this->FullPath.empty() && this->Language.empty()) {
    // If a known extension is given or a known full path is given
    // then trust that the current extension is sufficient to
    // determine the language.  This will fail only if the user
    // specifies a full path to the source but leaves off the
    // extension, which is kind of weird.
    if (this->Location.ExtensionIsAmbiguous() &&
        this->Location.DirectoryIsAmbiguous()) {
      // Finalize the file location to get the extension and set the
      // language.
      this->GetFullPath();
    } else {
      // Use the known extension to get the language if possible.
      std::string ext =
        cmSystemTools::GetFilenameLastExtension(this->Location.GetName());
      this->CheckLanguage(ext);
    }
  }

  // Now try to determine the language.
  return static_cast<cmSourceFile const*>(this)->GetLanguage();
}

std::string cmSourceFile::GetLanguage() const
{
  // If the language was set explicitly by the user then use it.
  if (const char* lang = this->GetProperty(propLANGUAGE)) {
    return lang;
  }

  // If the language was determined from the source file extension use it.
  if (!this->Language.empty()) {
    return this->Language;
  }

  // The language is not known.
  return "";
}

cmSourceFileLocation const& cmSourceFile::GetLocation() const
{
  return this->Location;
}

std::string const& cmSourceFile::GetFullPath(std::string* error)
{
  if (this->FullPath.empty()) {
    if (this->FindFullPath(error)) {
      this->CheckExtension();
    }
  }
  return this->FullPath;
}

std::string const& cmSourceFile::GetFullPath() const
{
  return this->FullPath;
}

bool cmSourceFile::FindFullPath(std::string* error)
{
  // If this method has already failed once do not try again.
  if (this->FindFullPathFailed) {
    return false;
  }

  // If the file is generated compute the location without checking on
  // disk.
  if (this->GetPropertyAsBool(propGENERATED)) {
    // The file is either already a full path or is relative to the
    // build directory for the target.
    this->Location.DirectoryUseBinary();
    this->FullPath = this->Location.GetDirectory();
    this->FullPath += "/";
    this->FullPath += this->Location.GetName();
    return true;
  }

  // The file is not generated.  It must exist on disk.
  cmMakefile const* mf = this->Location.GetMakefile();
  const char* tryDirs[3] = { nullptr, nullptr, nullptr };
  if (this->Location.DirectoryIsAmbiguous()) {
    tryDirs[0] = mf->GetCurrentSourceDirectory().c_str();
    tryDirs[1] = mf->GetCurrentBinaryDirectory().c_str();
  } else {
    tryDirs[0] = "";
  }

  cmake const* const cmakeInst = mf->GetCMakeInstance();
  std::vector<std::string> const& srcExts = cmakeInst->GetSourceExtensions();
  std::vector<std::string> const& hdrExts = cmakeInst->GetHeaderExtensions();
  for (const char* const* di = tryDirs; *di; ++di) {
    std::string tryPath = this->Location.GetDirectory();
    if (!tryPath.empty()) {
      tryPath += "/";
    }
    tryPath += this->Location.GetName();
    tryPath = cmSystemTools::CollapseFullPath(tryPath, *di);
    if (this->TryFullPath(tryPath, "")) {
      return true;
    }
    for (std::string const& ext : srcExts) {
      if (this->TryFullPath(tryPath, ext)) {
        return true;
      }
    }
    for (std::string const& ext : hdrExts) {
      if (this->TryFullPath(tryPath, ext)) {
        return true;
      }
    }
  }

  std::ostringstream e;
  std::string missing = this->Location.GetDirectory();
  if (!missing.empty()) {
    missing += "/";
  }
  missing += this->Location.GetName();
  e << "Cannot find source file:\n  " << missing << "\nTried extensions";
  for (std::string const& srcExt : srcExts) {
    e << " ." << srcExt;
  }
  for (std::string const& ext : hdrExts) {
    e << " ." << ext;
  }
  if (error) {
    *error = e.str();
  } else {
    this->Location.GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
                                               e.str());
  }
  this->FindFullPathFailed = true;
  return false;
}

bool cmSourceFile::TryFullPath(const std::string& path, const std::string& ext)
{
  std::string tryPath = path;
  if (!ext.empty()) {
    tryPath += ".";
    tryPath += ext;
  }
  if (cmSystemTools::FileExists(tryPath)) {
    this->FullPath = tryPath;
    return true;
  }
  return false;
}

void cmSourceFile::CheckExtension()
{
  // Compute the extension.
  std::string realExt =
    cmSystemTools::GetFilenameLastExtension(this->FullPath);
  if (!realExt.empty()) {
    // Store the extension without the leading '.'.
    this->Extension = realExt.substr(1);
  }

  // Look for object files.
  if (this->Extension == "obj" || this->Extension == "o" ||
      this->Extension == "lo") {
    this->SetProperty("EXTERNAL_OBJECT", "1");
  }

  // Try to identify the source file language from the extension.
  if (this->Language.empty()) {
    this->CheckLanguage(this->Extension);
  }
}

void cmSourceFile::CheckLanguage(std::string const& ext)
{
  // Try to identify the source file language from the extension.
  cmMakefile const* mf = this->Location.GetMakefile();
  cmGlobalGenerator* gg = mf->GetGlobalGenerator();
  std::string l = gg->GetLanguageFromExtension(ext.c_str());
  if (!l.empty()) {
    this->Language = l;
  }
}

bool cmSourceFile::Matches(cmSourceFileLocation const& loc)
{
  return this->Location.Matches(loc);
}

void cmSourceFile::SetProperty(const std::string& prop, const char* value)
{
  this->Properties.SetProperty(prop, value);

  // Update IsGenerated flag
  if (prop == propGENERATED) {
    this->IsGenerated = cmSystemTools::IsOn(value);
  }
}

void cmSourceFile::AppendProperty(const std::string& prop, const char* value,
                                  bool asString)
{
  this->Properties.AppendProperty(prop, value, asString);

  // Update IsGenerated flag
  if (prop == propGENERATED) {
    this->IsGenerated = this->GetPropertyAsBool(propGENERATED);
  }
}

const char* cmSourceFile::GetPropertyForUser(const std::string& prop)
{
  // This method is a consequence of design history and backwards
  // compatibility.  GetProperty is (and should be) a const method.
  // Computed properties should not be stored back in the property map
  // but instead reference information already known.  If they need to
  // cache information in a mutable ivar to provide the return string
  // safely then so be it.
  //
  // The LOCATION property is particularly problematic.  The CMake
  // language has very loose restrictions on the names that will match
  // a given source file (for historical reasons).  Implementing
  // lookups correctly with such loose naming requires the
  // cmSourceFileLocation class to commit to a particular full path to
  // the source file as late as possible.  If the users requests the
  // LOCATION property we must commit now.
  if (prop == propLOCATION) {
    // Commit to a location.
    this->GetFullPath();
  }

  // Perform the normal property lookup.
  return this->GetProperty(prop);
}

const char* cmSourceFile::GetProperty(const std::string& prop) const
{
  // Check for computed properties.
  if (prop == propLOCATION) {
    if (this->FullPath.empty()) {
      return nullptr;
    }
    return this->FullPath.c_str();
  }

  const char* retVal = this->Properties.GetPropertyValue(prop);
  if (!retVal) {
    cmMakefile const* mf = this->Location.GetMakefile();
    const bool chain =
      mf->GetState()->IsPropertyChained(prop, cmProperty::SOURCE_FILE);
    if (chain) {
      return mf->GetProperty(prop, chain);
    }
  }

  return retVal;
}

const char* cmSourceFile::GetSafeProperty(const std::string& prop) const
{
  const char* ret = this->GetProperty(prop);
  if (!ret) {
    return "";
  }
  return ret;
}

bool cmSourceFile::GetPropertyAsBool(const std::string& prop) const
{
  return cmSystemTools::IsOn(this->GetProperty(prop));
}

cmCustomCommand* cmSourceFile::GetCustomCommand()
{
  return this->CustomCommand;
}

cmCustomCommand const* cmSourceFile::GetCustomCommand() const
{
  return this->CustomCommand;
}

void cmSourceFile::SetCustomCommand(cmCustomCommand* cc)
{
  cmCustomCommand* old = this->CustomCommand;
  this->CustomCommand = cc;
  delete old;
}
