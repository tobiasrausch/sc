/*
============================================================================
Coral: COpy-numbeR ALterations
============================================================================
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#ifndef SEGMENT_H
#define SEGMENT_H

#define _SECURE_SCL 0
#define _SCL_SECURE_NO_WARNINGS
#include <iostream>
#include <vector>
#include <fstream>
#include <complex>

#define BOOST_DISABLE_ASSERTS
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/moment.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/multi_array.hpp>
#include <boost/tokenizer.hpp>
#include <boost/filesystem.hpp>
#include <boost/progress.hpp>
#include <boost/unordered_map.hpp>

#include "config.h"
#include "matrix.h"
#include "gflars.h"
#include "dpseg.h"
#include "util.h"

namespace coralns
{

  struct SegmentConfig {
    uint32_t k;
    float ploidy;
    double epsilon;
    double dpthreshold;
    boost::filesystem::path outfile;
    boost::filesystem::path signal;
  };
  


  struct NormalizedBinCounts {
    typedef double TPrecision;
    typedef boost::multi_array<TPrecision, 2> TSignalMatrix;
    typedef std::pair<uint32_t, uint32_t> TStartEnd;
    typedef std::vector<TStartEnd> TIntervals;
    std::string chr;
    uint32_t rows;
    uint32_t cols;
    TIntervals itv;
    TSignalMatrix sm;
    
    NormalizedBinCounts() : chr(""), rows(0), cols(0) {}
  };


  struct SmoothSignal {
    typedef Recap::TPrecision TPrecision;
    typedef std::vector<uint32_t> TIndexVector;
    typedef boost::multi_array<TPrecision, 2> TSignalMatrix;

    TIndexVector jumps;
    TSignalMatrix smooth;
    TSignalMatrix updown;
  };


  template<typename TVector>
  inline void
  meanSd(TVector const& vec, double& mean, double& sd) {
    boost::accumulators::accumulator_set<double, boost::accumulators::stats<boost::accumulators::tag::variance(boost::accumulators::lazy)> > acc;
    for_each(vec.begin(), vec.end(), boost::bind<void>(boost::ref(acc), _1));
    mean = boost::accumulators::mean(acc);
    sd = std::sqrt(boost::accumulators::variance(acc));
  }

  template<typename TSignalMatrix>  
  inline void
  undoBreaks(TSignalMatrix const& sm, std::vector<uint32_t> const& jumps, std::vector<uint32_t>& outj, double const scale) {
    typedef Recap::TPrecision TPrecision;
    uint32_t nrow = sm.shape()[0];
    uint32_t ncol = sm.shape()[1];

    uint32_t k = jumps.size();
    std::vector<int32_t> b(jumps.begin(), jumps.end());
    b.push_back(-1);
    b.push_back(nrow - 1);
    std::sort(b.begin(), b.end());

    // Collect mean and sd values
    boost::multi_array<double, 2> meanVal;
    meanVal.resize(boost::extents[k+1][ncol]);
    boost::multi_array<double, 2> sdVal;
    sdVal.resize(boost::extents[k+1][ncol]);
    boost::multi_array<int32_t, 2> marker;
    marker.resize(boost::extents[k+1][ncol]);
    for(uint32_t i = 0; i <= k; ++i) {
      uint32_t istart = b[i] + 1;
      uint32_t iend = b[i+1] + 1;
      for(uint32_t j = 0; j < ncol; ++j) {
	boost::accumulators::accumulator_set<double, boost::accumulators::stats<boost::accumulators::tag::variance(boost::accumulators::lazy)> > acc;
	TPrecision lastVal = 0;
	int32_t n = 0;
	for(uint32_t ki = istart; ki<iend; ++ki) {
	  if ((ki == istart) || (lastVal != sm[ki][j])) {
	    acc(sm[ki][j]);
	    lastVal = sm[ki][j];
	    ++n;
	  }
	}
	meanVal[i][j] = boost::accumulators::mean(acc);
	sdVal[i][j] = std::sqrt(boost::accumulators::variance(acc));
	marker[i][j] = n;
      }
    }

    for(uint32_t i = 0; i < k; ++i) {
      bool keepBreak = false;
      for(uint32_t j = 0; j < ncol; ++j) {
	double diffMean = std::abs(meanVal[i][j] - meanVal[i+1][j]);
	if ((marker[i][j] >= 5) && (marker[i+1][j] >= 5)) {
	  if ((diffMean > scale * sdVal[i][j]) || (diffMean > scale * sdVal[i+1][j])) {
	    keepBreak = true;
	    break;
	  }
	}
      }
      if (keepBreak) outj.push_back(b[i+1]);
    }      
  }
  
  template<typename TSignalMatrix>
  inline void
  smoothsignal(TSignalMatrix const& sm, std::vector<uint32_t> const& jumps, SmoothSignal& res) {
    typedef Recap::TPrecision TPrecision;
    uint32_t nrow = sm.shape()[0];
    uint32_t ncol = sm.shape()[1];
    
    std::vector<int32_t> b(jumps.begin(), jumps.end());
    b.push_back(-1);
    b.push_back(nrow-1);
    std::sort(b.begin(), b.end());

    res.jumps.clear();
    for(uint32_t i = 1; i < b.size(); ++i) res.jumps.push_back(b[i]);
    uint32_t k = res.jumps.size();
    res.smooth.resize(boost::extents[k][ncol]);
    for(uint32_t i = 0; i < k; ++i) {
      uint32_t istart = b[i] + 1;
      uint32_t iend = b[i+1] + 1;
      for(uint32_t j = 0; j < ncol; ++j) {
	std::vector<TPrecision> avg;
	TPrecision lastVal = 0;
	for(uint32_t ki = istart; ki<iend; ++ki) {
	  if ((ki == istart) || (lastVal != sm[ki][j])) {
	    //if (!j) std::cout << ',' << sm[ki][j] + 2;
	    //else std::cout << ',' << sm[ki][j] + 1;
	    avg.push_back(sm[ki][j]);
	    lastVal = sm[ki][j];
	  }
	}
	std::sort(avg.begin(), avg.end());
	res.smooth[i][j] = avg[avg.size() / 2];
	//if (!j) std::cout << j << ':' << res.smooth[i][j] + 2 << std::endl;
	//else std::cout << j << ':' << res.smooth[i][j] + 1 << std::endl;
      }
    }
  }

  inline int32_t
  runSegmentation(SegmentConfig const& c, std::vector<NormalizedBinCounts> const& cnbc) {

    // Output file
    boost::iostreams::filtering_ostream dataOutS;
    dataOutS.push(boost::iostreams::gzip_compressor());
    dataOutS.push(boost::iostreams::file_sink(c.outfile.string().c_str(), std::ios_base::out | std::ios_base::binary));
    dataOutS << "chr\tstart\tend\tcn" << std::endl;
    
    // Iterate chromosomes
    for(uint32_t refIndex = 0; refIndex < cnbc.size(); ++refIndex) {
      // Segmentation
      Recap res;
      gflars(c, cnbc[refIndex].sm, res);
      
      // Debug
      //for(uint32_t i = 0; i < c.k; ++i ) std::cout << "RefIndex " << refIndex << ", Iteration " << i << ", " << res.lambda[i] << ", " << res.jump[i] << std::endl;
      
      // DP
      dpseg(c, cnbc[refIndex].sm, res);

      // SD undo with 1 * SD
      /*
      uint32_t newBreakSize = res.kbestjump.size();
      uint32_t oldBreakSize = newBreakSize + 1;
      while (newBreakSize != oldBreakSize) {
	std::vector<uint32_t> finalJumps;
	oldBreakSize = newBreakSize;
	undoBreaks(cnbc[refIndex].sm, res.kbestjump, finalJumps, 1.0);
	res.kbestjump = finalJumps;
	newBreakSize = res.kbestjump.size();
      }
      */
      
      // Smooth signal
      SmoothSignal smoo;
      smoothsignal(cnbc[refIndex].sm, res.kbestjump, smoo);
      //expandpiecewiseconstant(smoo.jumps, smoo.smooth, res.smooth);
      
      // Output matrix
      uint32_t istart = cnbc[refIndex].itv[0].first;
      for(uint32_t i = 0; i<smoo.jumps.size(); ++i) {
	if (i) istart = cnbc[refIndex].itv[smoo.jumps[i-1]+1].first;
	uint32_t iend = cnbc[refIndex].itv[smoo.jumps[i]].second;
	double avgCN = 0;
	for(uint32_t col = 0; col < cnbc[refIndex].cols; ++col) avgCN += smoo.smooth[i][col];
	avgCN /= (double) cnbc[refIndex].cols;
	dataOutS << cnbc[refIndex].chr << '\t' << istart << '\t' << iend << '\t' << (avgCN + c.ploidy) << std::endl;
      }
    }
    dataOutS.pop();
  
    return 0;
  }


  inline int32_t
  segmentCovBaf(SegmentConfig const& c) {
    // Parse matrix
    typedef std::vector<NormalizedBinCounts> TChrBinCounts;
    TChrBinCounts cnbc;
    if (!(boost::filesystem::exists(c.signal) && boost::filesystem::is_regular_file(c.signal) && boost::filesystem::file_size(c.signal))) {
      std::cerr << "Signal matrix is missing: " << c.signal.string() << std::endl;
      return 1;
    } else {
      // Get intervals & matrix dimensions for each chromosome
      std::ifstream file(c.signal.string().c_str(), std::ios_base::in | std::ios_base::binary);
      boost::iostreams::filtering_streambuf<boost::iostreams::input> dataIn;
      dataIn.push(boost::iostreams::gzip_decompressor());
      dataIn.push(file);
      std::istream instream(&dataIn);
      std::string sigdata;
      NormalizedBinCounts nbc;
      while(std::getline(instream, sigdata)) {
	if (sigdata.size()) {
	  typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	  boost::char_separator<char> sep(" \t,;");
	  Tokenizer tokens(sigdata, sep);
	  Tokenizer::iterator tokIter = tokens.begin();
	  if (tokIter!=tokens.end()) {
	    std::string chrName = *tokIter++;
	    if ((tokIter == tokens.end()) || (*tokIter == "start")) continue; //header
	    ++tokIter; // start
	    if (tokIter == tokens.end()) continue;
	    ++tokIter; // end
	    int32_t col = 0;
	    for(;tokIter != tokens.end(); ++tokIter) ++col;
	    if (nbc.chr != chrName) {
	      if ((nbc.cols) && (nbc.rows)) cnbc.push_back(nbc);
	      nbc.chr = chrName;
	      nbc.cols = col;
	      nbc.rows = 0;
	    }
	    ++nbc.rows;
	  }
	}
      }
      if ((nbc.cols) && (nbc.rows)) cnbc.push_back(nbc);
      dataIn.pop();
    }
    if (cnbc.empty()) {
      std::cerr << "Signal matrix format is chr, start, end, signal, ..." << std::endl;
      return 1;
    } else {
      for (uint32_t i = 0; i < cnbc.size(); ++i) {
	std::cout << "Matrix dimensions for " << cnbc[i].chr << " are: " << cnbc[i].rows << "x" << cnbc[i].cols << std::endl;
      }
    }

    typedef NormalizedBinCounts::TPrecision TPrecision;
    // Parse signal matrix
    uint32_t refIndex = 0;
    std::vector<TPrecision> lastCN(cnbc[refIndex].cols, 0);
    cnbc[refIndex].sm.resize(boost::extents[cnbc[refIndex].rows][cnbc[refIndex].cols]);
    cnbc[refIndex].itv.resize(cnbc[refIndex].rows);

    std::ifstream file(c.signal.string().c_str(), std::ios_base::in | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> dataIn;
    dataIn.push(boost::iostreams::gzip_decompressor());
    dataIn.push(file);
    std::istream instream(&dataIn);
    std::string sigdata;
    uint32_t row = 0;
    while(std::getline(instream, sigdata)) {
      while ((row >= cnbc[refIndex].rows) && (refIndex + 1 < cnbc.size())) {
	++refIndex;
	for(uint32_t i = 0; i<cnbc[refIndex].cols;++i) lastCN[i] = 0;
	cnbc[refIndex].sm.resize(boost::extents[cnbc[refIndex].rows][cnbc[refIndex].cols]);
	cnbc[refIndex].itv.resize(cnbc[refIndex].rows);
	row = 0;
      }
      if (row < cnbc[refIndex].rows) {
	typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	boost::char_separator<char> sep(" \t,;");
	Tokenizer tokens(sigdata, sep);
	Tokenizer::iterator tokIter = tokens.begin();
	if (tokIter!=tokens.end()) {
	  std::string chrName = *tokIter++;
	  if (tokIter!=tokens.end()) {
	    if (*tokIter == "start") continue; //header
	    cnbc[refIndex].itv[row].first = boost::lexical_cast<uint32_t>(*tokIter++);
	    cnbc[refIndex].itv[row].second = boost::lexical_cast<uint32_t>(*tokIter++);
	    uint32_t colIdx = 0;
	    for (;tokIter!=tokens.end();++tokIter, ++colIdx) {
	      std::string cn = *tokIter;
	      if ((cn == "NaN") || (cn == "NA")) cnbc[refIndex].sm[row][colIdx] = lastCN[colIdx];
	      else {
		cnbc[refIndex].sm[row][colIdx] = boost::lexical_cast<TPrecision>(cn) - c.ploidy;
		lastCN[colIdx] = cnbc[refIndex].sm[row][colIdx];
	      }
	    }
	  }
	}
	++row;
      }
    }
    dataIn.pop();


    // Debug
    /*
    for(uint32_t refIndex = 0; refIndex < cnbc.size(); ++refIndex) {
      for(uint32_t row = 0; row < cnbc[refIndex].rows; ++row) {
	std::cout << cnbc[refIndex].chr << ':';
	for(uint32_t col = 0; col < cnbc[refIndex].cols; ++col) {
	  std::cout << cnbc[refIndex].sm[row][col]  << ',';
	}
	std::cout << std::endl;
      }
    }
    */

    return runSegmentation(c, cnbc);
  }

  
  int segment(int argc, char **argv) {
    SegmentConfig c;

    // Parameter
    boost::program_options::options_description generic("Generic options");
    generic.add_options()
      ("help,?", "show help message")
      ("ploidy,y", boost::program_options::value<float>(&c.ploidy)->default_value(2), "baseline ploidy")
      ("dpthreshold,d", boost::program_options::value<double>(&c.dpthreshold)->default_value(0.5), "DP threshold")
      ("epsilon,e", boost::program_options::value<double>(&c.epsilon)->default_value(1e-9), "epsilon error")
      ("kchange,k", boost::program_options::value<uint32_t>(&c.k)->default_value(300), "change points per chr")
      ("outfile,o", boost::program_options::value<boost::filesystem::path>(&c.outfile)->default_value("segment.gz"), "output file")
      ;

    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
      ("input-file", boost::program_options::value<boost::filesystem::path>(&c.signal), "input signal matrix")
      ;

    boost::program_options::positional_options_description pos_args;
    pos_args.add("input-file", -1);

    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic).add(hidden);
    boost::program_options::options_description visible_options;
    visible_options.add(generic);
    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
    boost::program_options::notify(vm);

    // Check command line arguments
    if ((vm.count("help")) || (!vm.count("input-file"))) {
      std::cout << "Usage: " << argv[0] << " [OPTIONS] <signal.tsv>" << std::endl;
      std::cout << visible_options << "\n";
      return 1;
    }

    // Show cmd
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
    for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
    std::cout << std::endl;
    
    int32_t retVal = segmentCovBaf(c);
    if (retVal != 0) return retVal;

    // End
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;

    return 0;

  }

}

#endif
