// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pal_trust.h"

static bool CheckTrustMatch(SecCertificateRef cert,
                            SecTrustSettingsDomain domain,
                            SecTrustSettingsResult result,
                            OSStatus* pOSStatus)
{
    CFArrayRef settings = nullptr;
    *pOSStatus = SecTrustSettingsCopyTrustSettings(cert, domain, &settings);
    bool isMatch = false;

    if (*pOSStatus == noErr && settings != nullptr)
    {
        CFIndex count = CFArrayGetCount(settings);

        if (count == 0)
        {
            *pOSStatus = noErr;
            // An empty array means that it counts as "Trust Root",
            // so we match if (and only if) we were asking for that.
            isMatch = (result == kSecTrustSettingsResultTrustRoot);
        }
        else
        {
            auto dictionaryTypeId = CFDictionaryGetTypeID();
            auto numberTypeId = CFNumberGetTypeID();

            for (CFIndex i = 0; i < count; i++)
            {
                CFTypeRef obj = CFArrayGetValueAtIndex(settings, i);

                if (CFGetTypeID(obj) != dictionaryTypeId)
                {
                    continue;
                }

                CFDictionaryRef dict = reinterpret_cast<CFDictionaryRef>(obj);

                if (CFDictionaryGetCount(dict) > 1)
                {
                    // This dictionary has constraints. A particular SecTrust evaluation
                    // might or might not make it apply. If the only extra key was a policy
                    // restriction, then matching on the basic X.509 policy might be fair,
                    // but it's not an obvious check with little expectation of applying.
                    //
                    // This may result in X509Chain reporting valid when enumerating the
                    // two root stores doesn't agree.
                    continue;
                }

                CFTypeRef val = CFDictionaryGetValue(dict, kSecTrustSettingsResult);

                if (val != nullptr && CFGetTypeID(val) == numberTypeId)
                {
                    CFNumberRef cfNum = reinterpret_cast<CFNumberRef>(val);
                    SecTrustSettingsResult trustValue = {};

                    if (CFNumberGetValue(cfNum, kCFNumberSInt32Type, &trustValue))
                    {
                        isMatch = (trustValue == result);
                        break;
                    }
                }
            }
        }
    }

    if (settings != nullptr)
        CFRelease(settings);

    return isMatch;
}

static int32_t EnumerateTrust(SecTrustSettingsDomain domain,
                              SecTrustSettingsResult result,
                              CFMutableArrayRef* pCertsRef,
                              int32_t* pOSStatus)
{
    if (pOSStatus != nullptr)
        *pOSStatus = noErr;

    if (pCertsRef == nullptr || pOSStatus == nullptr)
        return -1;

    CFMutableArrayRef outArray;

    if (*pCertsRef != nullptr)
    {
        outArray = *pCertsRef;
    }
    else
    {
        outArray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        *pCertsRef = outArray;
    }

    if (outArray == nullptr)
    {
        *pOSStatus = errSecAllocate;
        return 0;
    }

    CFArrayRef certsWithTrusts = nullptr;
    *pOSStatus = SecTrustSettingsCopyCertificates(domain, &certsWithTrusts);

    if (*pOSStatus == noErr)
    {
        CFIndex count = CFArrayGetCount(certsWithTrusts);
        auto certTypeId = SecCertificateGetTypeID();

        for (CFIndex i = 0; i < count; i++)
        {
            CFTypeRef obj = CFArrayGetValueAtIndex(certsWithTrusts, i);

            if (CFGetTypeID(obj) != certTypeId)
            {
                continue;
            }

            SecCertificateRef cert = reinterpret_cast<SecCertificateRef>(const_cast<void*>(obj));
            bool isMatch = CheckTrustMatch(cert, domain, result, pOSStatus);

            if (*pOSStatus != noErr)
            {
                break;
            }

            if (isMatch)
            {
                CFArrayAppendValue(outArray, cert);
            }
        }
    }
    else if (*pOSStatus == errSecNoTrustSettings)
    {
        // If there are no trust settings at the specified domain,
        // return the empty list as OK.
        *pOSStatus = noErr;
    }

    if (certsWithTrusts != nullptr)
    {
        CFRelease(certsWithTrusts);
    }

    int32_t ret = *pOSStatus == noErr;

    // Note that on a second call the array from the first call will get freed
    // if an error is encountered.
    if (ret == 0 || CFArrayGetCount(outArray) == 0)
    {
        CFRelease(outArray);
        *pCertsRef = nullptr;
    }

    return ret;
}

extern "C" int32_t AppleCryptoNative_StoreEnumerateUserRoot(CFArrayRef* pCertsOut, int32_t* pOSStatusOut)
{
    if (pCertsOut != nullptr)
        *pCertsOut = nullptr;

    return EnumerateTrust(kSecTrustSettingsDomainUser,
                          kSecTrustSettingsResultTrustRoot,
                          const_cast<CFMutableArrayRef*>(pCertsOut),
                          pOSStatusOut);
}

extern "C" int32_t AppleCryptoNative_StoreEnumerateMachineRoot(CFArrayRef* pCertsOut, int32_t* pOSStatusOut)
{
    if (pCertsOut != nullptr)
        *pCertsOut = nullptr;

    CFMutableArrayRef* pCertsRef = const_cast<CFMutableArrayRef*>(pCertsOut);

    int32_t ret =
        EnumerateTrust(kSecTrustSettingsDomainAdmin, kSecTrustSettingsResultTrustRoot, pCertsRef, pOSStatusOut);

    if (ret == 1)
    {
        ret = EnumerateTrust(kSecTrustSettingsDomainSystem, kSecTrustSettingsResultTrustRoot, pCertsRef, pOSStatusOut);
    }

    return ret;
}

extern "C" int32_t AppleCryptoNative_StoreEnumerateUserDisallowed(CFArrayRef* pCertsOut, int32_t* pOSStatusOut)
{
    if (pCertsOut != nullptr)
        *pCertsOut = nullptr;

    return EnumerateTrust(kSecTrustSettingsDomainUser,
                          kSecTrustSettingsResultDeny,
                          const_cast<CFMutableArrayRef*>(pCertsOut),
                          pOSStatusOut);
}

extern "C" int32_t AppleCryptoNative_StoreEnumerateMachineDisallowed(CFArrayRef* pCertsOut, int32_t* pOSStatusOut)
{
    if (pCertsOut != nullptr)
        *pCertsOut = nullptr;

    CFMutableArrayRef* pCertsRef = const_cast<CFMutableArrayRef*>(pCertsOut);

    int32_t ret = EnumerateTrust(kSecTrustSettingsDomainAdmin, kSecTrustSettingsResultDeny, pCertsRef, pOSStatusOut);

    if (ret == 1)
    {
        ret = EnumerateTrust(kSecTrustSettingsDomainSystem, kSecTrustSettingsResultDeny, pCertsRef, pOSStatusOut);
    }

    return ret;
}
