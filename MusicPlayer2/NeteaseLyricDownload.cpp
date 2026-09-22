#include "stdafx.h"
#include "NeteaseLyricDownload.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

std::wstring CNeteaseLyricDownload::GetSearchUrl(const std::wstring& key_words, int result_count)
{
	CString url;
	url.Format(L"http://music.163.com/api/search/get/?s=%s&limit=%d&type=1&offset=0", key_words.c_str(), result_count);
	return url.GetString();
}

void CNeteaseLyricDownload::DisposeSearchResult(vector<ItemInfo>& down_list, const wstring& search_result, int result_count)
{
	down_list.clear();

	try
	{
		json data = json::parse(search_result);

		if (data.contains("result") && data["result"].contains("songs") && data["result"]["songs"].is_array())
		{
			auto& songs = data["result"]["songs"];

			for (const auto& song_item : songs)
			{
				ItemInfo item;

				item.id = std::to_wstring(song_item.value("id", 0LL));
				item.title = CCommon::StrToUnicode(song_item.value("name", ""), CodeType::UTF8);
				item.duration = song_item.value("duration", 0);

				if (song_item.contains("album") && song_item["album"].is_object())
				{
					item.album = CCommon::StrToUnicode(song_item["album"].value("name", ""), CodeType::UTF8);
				}

				if (song_item.contains("artists") && song_item["artists"].is_array())
				{
					std::wstring artists_str;
					for (const auto& artist_item : song_item["artists"])
					{
						if (artist_item.is_object() && artist_item.contains("name"))
						{
							if (!artists_str.empty())
							{
								artists_str += L'/';
							}
							artists_str += CCommon::StrToUnicode(artist_item.value("name", ""), CodeType::UTF8);
						}
					}
					item.artist = artists_str;
				}

				CInternetCommon::DeleteStrSlash(item.title);
				CInternetCommon::DeleteStrSlash(item.artist);
				CInternetCommon::DeleteStrSlash(item.album);
				down_list.push_back(item);
			}
		}
	}
	catch (const std::exception& e)
	{
		TRACE(L"NeteaseLyricDownload JSON parse error: %hs\n", e.what());
	}
}

std::wstring CNeteaseLyricDownload::GetAlbumCoverURL(const wstring& song_id)
{
	if (song_id.empty())
		return wstring();
	//获取专辑封面接口的URL
	wchar_t buff[256];
	swprintf_s(buff, L"http://music.163.com/api/song/detail/?id=%s&ids=%%5B%s%%5D&csrf_token=", song_id.c_str(), song_id.c_str());
	wstring contents;
	//将URL内容保存到内存
	if (!CInternetCommon::GetURL(wstring(buff), contents))
		return wstring();
#ifdef _DEBUG
	ofstream out_put{ L".\\cover_down.log", std::ios::binary };
	out_put << CCommon::UnicodeToStr(contents, CodeType::UTF8);
	out_put.close();
#endif // _DEBUG

	size_t index;
	index = contents.find(L"\"album\"");
	if (index == wstring::npos)
		return wstring();
	index = contents.find(L"\"picUrl\"", index + 7);
	if (index == wstring::npos)
		return wstring();
	wstring url;
	size_t index1;
	index1 = contents.find(L'\"', index + 10);
	url = contents.substr(index + 10, index1 - index - 10);

	return url;
}

std::wstring CNeteaseLyricDownload::GetOnlineUrl(const wstring& song_id)
{
	std::wstring song_url{ L"http://music.163.com/#/song?id=" + song_id };
	return song_url;
}

int CNeteaseLyricDownload::RequestSearch(const std::wstring& url, std::wstring& result)
{
	return CInternetCommon::HttpPost(url, result);
}

bool CNeteaseLyricDownload::DownloadLyric(const wstring& song_id, wstring& result, bool download_translate)
{
	std::wstring lyric_url;
	if (!download_translate)
		lyric_url = L"http://music.163.com/api/song/media?id=" + song_id;
	else
		lyric_url = L"http://music.163.com/api/song/lyric?os=osx&id=" + song_id + L"&lv=-1&kv=-1&tv=-1";
	return CInternetCommon::GetURL(lyric_url, result);
}

bool CNeteaseLyricDownload::DisposeLryic(wstring& lyric_str, bool download_translate)
{
        // 响应是 JSON：lyric 是原文，tlyric 是译文。旧实现手工掐掉结尾再逐字符反转义，
        // 响应短一点就会越界读，译文也只是 JSON 残骸混进歌词碰巧能用。
        // 现在按 JSON 解析，和 QQ 音乐那个一个做法；译文整段跟在原文后面，
        // 时间戳相同的行会被 CLyrics 配成原文加译文。
        try
        {
                nlohmann::json res_json = nlohmann::json::parse(CCommon::UnicodeToStr(lyric_str, CodeType::UTF8));
                lyric_str = CCommon::StrToUnicode(res_json.value("lyric", std::string()), CodeType::UTF8);
                if (lyric_str.empty())
                        return false;
                if (download_translate)
                {
                        std::wstring trans = CCommon::StrToUnicode(res_json.value("tlyric", std::string()), CodeType::UTF8);
                        if (!trans.empty())
                        {
                                lyric_str += L"\r\n";
                                lyric_str += trans;
                        }
                }
        }
        catch (const std::exception& e)
        {
                TRACE(L"NeteaseLyricDownload dispose error: %hs\n", e.what());
                return false;
        }
        return true;
}
