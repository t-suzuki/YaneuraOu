﻿#include <sstream>
#include <queue>

#include "shogi.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "misc.h"

using namespace std;

// Positionクラスがそこにいたるまでの手順(捕獲された駒など)を保持しておかないと千日手判定が出来ないので
// StateInfo型のstackのようなものが必要となるので、それをglobalに確保しておく。
Search::StateStackPtr SetupStates;

// ユーザーの実験用に開放している関数。
// USI拡張コマンドで"user"と入力するとこの関数が呼び出される。
// "user"コマンドの後続に指定されている文字列はisのほうに渡される。
extern void user_test(Position& pos, std::istringstream& is);

// USI拡張コマンドの"test"コマンドなど。
// サンプル用のコードを含めてtest.cppのほうに色々書いてあるのでそれを呼び出すために使う。
#ifdef ENABLE_TEST_CMD
extern void test_cmd(Position& pos, istringstream& is);
extern void perft(Position& pos, istringstream& is);
extern void generate_moves_cmd(Position& pos);
extern void bench_cmd(Position& pos, istringstream& is);
#endif

// 定跡を作るコマンド
#ifdef ENABLE_MAKEBOOK_CMD
namespace Book { extern void makebook_cmd(Position& pos, istringstream& is); }
#endif

// 協力詰めsolverモード
#ifdef    COOPERATIVE_MATE_SOLVER
#include "cooperate_mate/cooperative_mate_solver.h"
#endif

// Option設定が格納されたglobal object。
USI::OptionsMap Options;

namespace USI
{
// --------------------
//    読み筋の出力
// --------------------

  // スコアを歩の価値を100として正規化して出力する。
  std::string score_to_usi(Value v)
  {
    std::stringstream s;

    // 置換表上、値が確定していないことがある。
    if (v == VALUE_NONE)
      s << "none";

    else if (abs(v) < VALUE_MATE_IN_MAX_PLY)
      s << "cp " << v * 100 / int(Eval::PawnValue);
    else
      s << "mate " << (v > 0 ? VALUE_MATE - v - 1 : -VALUE_MATE - v + 1);

    return s.str();
  }
  
  std::string pv(const Position& pos, int iteration_depth, Value alpha, Value beta)
  {
    std::stringstream ss;
    int elapsed = Time.elapsed() + 1;
    
    const auto& rootMoves = pos.this_thread()->rootMoves;
    size_t PVIdx = pos.this_thread()->PVIdx;
    size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());

    uint64_t nodes_searched = Threads.nodes_searched();

    // MultiPVでは上位N個の候補手と読み筋を出力する必要がある。
    for (size_t i = 0; i < multiPV; ++i)
    {
      // この指し手のpvの更新が終わっているのか
      bool updated = (i <= PVIdx);

      if (iteration_depth == ONE_PLY && !updated)
        continue;

      int d   = updated ? iteration_depth : iteration_depth - 1;
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;

      if (ss.rdbuf()->in_avail()) // 1行目でないなら連結のための改行を出力
        ss << endl;

      // maxPlyは更新しない思考エンジンがあるので、0で初期化しておき、
      // dのほうが大きければそれをそのまま表示することで回避する。

      ss << "info"
        << " depth " << d
        << " seldepth " << max(d, pos.this_thread()->maxPly)
        << " score " << USI::score_to_usi(v);

      // これが現在探索中の指し手であるなら、それがlowerboundかupperboundかは表示させる
      if (i == PVIdx)
        ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      // 将棋所はmultipvに対応していないが、とりあえず出力はしておく。
      if (multiPV > 1)
        ss << " multipv " << (i + 1);

      ss << " nodes " << nodes_searched
        << " nps " << nodes_searched * 1000 / elapsed;

      // 置換表使用率。経過時間が短いときは意味をなさないので出力しない。
      if (elapsed > 1000)
        ss << " hashfull " << TT.hashfull();

      ss << " time " << elapsed
         << " pv";

#ifdef USE_TT_PV
      // 置換表からPVをかき集めてくるモード
      {
        auto pos_ = const_cast<Position*>(&pos);
        Move moves[MAX_PLY + 1];
        StateInfo si[MAX_PLY];
        moves[0] = rootMoves[i].pv[0];
        int ply = 0;
        while (ply < MAX_PLY && moves[ply] != MOVE_NONE)
        {
          pos_->check_info_update();
          pos_->do_move(moves[ply], si[ply]);
          ss << " " << moves[ply];
          bool found;
          auto tte = TT.probe(pos.state()->key(), found);
          ply++;
          moves[ply] = found ? tte->move() : MOVE_NONE;
        }
        while (ply > 0)
          pos_->undo_move(moves[--ply]);
      }
#else
      // rootMovesが自らPVを持っているモード

      for (Move m : rootMoves[i].pv)
        ss << " " << m;
#endif
    }

    return ss.str();
  }


// --------------------
//     USI::Option
// --------------------

  // この関数はUSI::init()から起動時に呼び出されるだけ。
  void Option::operator<<(const Option& o)
  {
    static size_t insert_order = 0;
    *this = o;
    idx = insert_order++; // idxは0から連番で番号を振る
  }

  // optionのdefault値を設定する。
  void init(OptionsMap& o)
  {
    // Hash上限。32bitモードなら2GB、64bitモードなら1024GB
    const int MaxHashMB = Is64Bit ? 1024 * 1024 : 2048;

    o["Threads"] << Option(4, 1, 128, [](auto& o) { Threads.read_usi_options(); });

    // USIプロトコルでは、"USI_Hash"と"USI_Ponder"なのだが、
    // 置換表サイズを変更しての自己対戦などをさせたいので、
    // 片方だけ変更できなければならない。
    // ゆえにGUIでの対局設定は無視して、思考エンジンの設定ダイアログのところで
    // 個別設定が出来るようにする。
    o["Hash"]    << Option(16, 1, MaxHashMB, [](auto&o) { TT.resize(o); });
    o["Ponder"]  << Option(false);

    // その局面での上位N個の候補手を調べる機能
    o["MultiPV"] << Option(1, 1, 800);

    // cin/coutの入出力をファイルにリダイレクトする
    o["WriteDebugLog"] << Option(false, [](auto& o) { start_logger(o); });

    // ネットワーク遅延時間[ms]
    o["NetworkDelay"] << Option(400, 0, 10000);

    // 引き分けを受け入れるスコア
    o["Contempt"] << Option(0, -30000, 30000);

    // 各エンジンがOptionを追加したいだろうから、コールバックする。
    USI::extra_option(o);
  }

  // USIプロトコル経由で値を設定されたときにそれをcurrentValueに反映させる。
  Option& Option::operator=(const string& v) {

    ASSERT_LV1(!type.empty());

    // 範囲外
    if ((type != "button" && v.empty())
      || (type == "check" && v != "true" && v != "false")
      || (type == "spin" && (stoi(v) < min || stoi(v) > max)))
      return *this;

    // ボタン型は値を設定するものではなく、単なるトリガーボタン。
    // ボタン型以外なら入力値をcurrentValueに反映させてやる。
    if (type != "button")
      currentValue = v;

    // 値が変化したのでハンドラを呼びだす。
    if (on_change)
      on_change(*this);

    return *this;
  }

  std::ostream& operator<<(std::ostream& os, const OptionsMap& om)
  {
    // idxの順番を守って出力する
    for (size_t idx = 0; idx < om.size();++idx)
      for(const auto& it:om)
        if (it.second.idx == idx)
        {
          const Option& o = it.second;
          os << "option name " << it.first << " type " << o.type;
          if (o.type != "button")
            os << " default " << o.defaultValue;
          if (o.type == "spin")
            os << " min " << o.min << " max " << o.max;
          os << endl;
          break;
        }

    return os;
  }
}

// --------------------
// USI関係のコマンド処理
// --------------------

// is_ready_cmd()を外部から呼び出せるようにしておく。(benchコマンドなどから呼び出したいため)
void is_ready()
{
  static bool first = true;

  // 評価関数の読み込みなど時間のかかるであろう処理はこのタイミングで行なう。
  // 起動時に時間のかかる処理をしてしまうと将棋所がタイムアウト判定をして、思考エンジンとしての認識をリタイアしてしまう。
  if (first)
  {
    // 評価関数の読み込み
    Eval::load_eval();

    first = false;
  }

  Search::clear();
}

// isreadyコマンド処理部
void is_ready_cmd()
{
  is_ready();
  sync_cout << "readyok" << sync_endl;
}

// "position"コマンド処理部
void position_cmd(Position& pos,istringstream& is)
{
  Move m;
  string token, sfen;

  is >> token;

  if (token == "startpos")
  {
    // 初期局面として初期局面のFEN形式の入力が与えられたとみなして処理する。
    sfen = SFEN_HIRATE;
    is >> token; // もしあるなら"moves"トークンを消費する。
  }
  // 局面がfen形式で指定されているなら、その局面を読み込む。
  // UCI(チェスプロトコル)ではなくUSI(将棋用プロトコル)だとここの文字列は"fen"ではなく"sfen"
  // この"sfen"という文字列は省略可能にしたいので..
  else {
    if (token != "sfen")
      sfen += token + " ";
    while (is >> token && token != "moves")
      sfen += token + " ";
  }

  pos.set(sfen);

  SetupStates = Search::StateStackPtr(new aligned_stack<StateInfo>);

  // 指し手のリストをパースする(あるなら)
  while (is >> token && (m = move_from_usi(pos, token)) != MOVE_NONE)
  {
    // 1手進めるごとにStateInfoが積まれていく。これは千日手の検出のために必要。
    // ToDoあとで考える。
    SetupStates->push(StateInfo());
    pos.do_move(m, SetupStates->top());
  }
}

// "setoption"コマンド応答。
void setoption_cmd(istringstream& is)
{
  string token, name, value;

  while (is >> token && token != "value")
    // "name"トークンはあってもなくても良いものとする。(手打ちでコマンドを打つときには省略したい)
    if (token != "name")
      // スペース区切りで長い名前のoptionを使うことがあるので2つ目以降はスペースを入れてやる
      name += (name.empty() ? "" : " ") + token;

  // valueの後ろ。スペース区切りで複数文字列が来ることがある。
  while (is >> token)
    value +=  (value.empty() ? "" : " ") + token;

  if (Options.count(name))
    Options[name] = value;
  else {
    // USI_HashとUSI_Ponderは無視してやる。
    if (name != "USI_Hash" && name != "USI_Ponder" )
      // この名前のoptionは存在しなかった
      sync_cout << "No such option: " << name << sync_endl;
  }
}

// go()は、思考エンジンがUSIコマンドの"go"を受け取ったときに呼び出される。
// この関数は、入力文字列から思考時間とその他のパラメーターをセットし、探索を開始する。
void go_cmd(const Position& pos, istringstream& is) {

  Search::LimitsType limits;
  string token;

  // 思考開始時刻の初期化。なるべく早い段階でこれを代入しておかないとサーバー時間との誤差が大きくなる。
  Time.init();
  
  // goコマンド、デバッグ時に使うが、そのときに"go btime XXX wtime XXX byoyomi XXX"と毎回入力するのが面倒なので
  // デフォルトで1秒読み状態で呼び出されて欲しい。
  limits.byoyomi[BLACK] = limits.byoyomi[WHITE] = 1000;

  while (is >> token)
  {
    // 探索すべき指し手。(探索開始局面から特定の初手だけ探索させるとき)
    if (token == "searchmoves")
      // 残りの指し手すべてをsearchMovesに突っ込む。
      while (is >> token)
        limits.searchmoves.push_back(move_from_usi(pos, token));

    // 先手、後手の残り時間。[ms]
    else if (token == "wtime")     is >> limits.time[WHITE];
    else if (token == "btime")     is >> limits.time[BLACK];

    // "go rtime 100"だと100～300[ms]思考する。
    else if (token == "rtime")     is >> limits.rtime;

    // 秒読み設定。
    else if (token == "byoyomi") {
      int t = 0;
      is >> t;

      // USIプロトコルで送られてきた秒読み時間より少なめに思考する設定
      // ※　通信ラグがあるときに、ここで少なめに思考しないとタイムアップになる可能性があるので。
    
      // t = std::max(t - Options["ByoyomiMinus"], Time::point(0));

      // USIプロトコルでは、これが先手後手同じ値だと解釈する。
      limits.byoyomi[BLACK] = limits.byoyomi[WHITE] = t;
    }
    // この探索深さで探索を打ち切る
    else if (token == "depth")     is >> limits.depth;

    // この探索ノード数で探索を打ち切る
    else if (token == "nodes")     is >> limits.nodes;

    // 詰み探索。"UCI"プロトコルではこのあとには手数が入っており、その手数以内に詰むかどうかを判定するが、
    // "USI"プロトコルでは、ここは探索のための時間制限に変更となっている。
    else if (token == "mate") {
      is >> token;
      if (token == "infinite")
        limits.mate = INT32_MAX;
      else
        is >> limits.mate;
    }

    // 時間無制限。
    else if (token == "infinite")  limits.infinite = 1;

    // ponderモードでの思考。
    else if (token == "ponder")    limits.ponder = 1;
  }

  Threads.start_thinking(pos, limits, Search::SetupStates);
}



// --------------------
// 　　USI応答部
// --------------------

// USI応答部本体
void USI::loop(int argc,char* argv[])
{
  // 探索開始局面(root)を格納するPositionクラス
  Position pos;
  string cmd,token;

  // 先行入力されているコマンド
  // コマンドは前から取り出すのでqueueを用いる。
  queue<string> cmds;

  // ファイルからコマンドの指定
  if (argc >= 3 && string(argv[1]) == "file")
  {
    vector<string> cmds0;
    read_all_lines(argv[2], cmds0);

    // queueに変換する。
    for (auto c : cmds0)
      cmds.push(c);

  } else {

    // 引数として指定されたものを一つのコマンドとして実行する機能
    // ただし、','が使われていれば、そこでコマンドが区切れているものとして解釈する。

    for (int i = 1; i < argc; ++i)
    {
      string s = argv[i];

      // sから前後のスペースを除去しないといけない。
      while (*s.rbegin() == ' ') s.pop_back();
      while (*s.begin() == ' ') s = s.substr(1,s.size()-1);

      if (s != ",")
        cmd += s + " ";
      else
      {
        cmds.push(cmd);
        cmd = "";
      }
    }
    if (cmd.size() != 0)
      cmds.push(cmd);
  }

  do
  {
    if (cmds.size() == 0)
    {
      if (!getline(cin, cmd)) // 入力が来るかEOFがくるまでここで待機する。
        cmd = "quit";
    } else {
      // 積んであるコマンドがあるならそれを実行する。
      // 尽きれば"quit"だと解釈してdoループを抜ける仕様にすることはできるが、
      // そうしてしまうとgoコマンド(これはノンブロッキングなので)の最中にquitが送られてしまう。
      cmd = cmds.front();
      cmds.pop();
    }

    istringstream is(cmd);

    token = "";
    is >> skipws >> token;

    if (token == "quit" || token == "stop")
    {
      Search::Signals.stop = true;

      // 思考を終えて寝てるかも知れないのでresume==trueにして呼び出してやる
      Threads.main()->start_searching(true);
    }

    // 与えられた局面について思考するコマンド
    else if (token == "go") { go_cmd(pos, is); break; }

    // (思考などに使うための)開始局面(root)を設定する
    else if (token == "position") position_cmd(pos, is);

    // 起動時いきなりこれが飛んでくるので速攻応答しないとタイムアウトになる。
    else if (token == "usi")
      sync_cout << "id name " << engine_info() << Options << "usiok" << sync_endl;

    // オプションを設定する
    else if (token == "setoption") setoption_cmd(is);

    // 思考エンジンの準備が出来たかの確認
    else if (token == "isready") is_ready_cmd();

    // ユーザーによる実験用コマンド。user.cppのuser()が呼び出される。
    else if (token == "user") user_test(pos,is);

    // 現在の局面を表示する。(デバッグ用)
    else if (token == "d") cout << pos << endl;

    // 指し手生成祭りの局面をセットする。
    else if (token == "matsuri") pos.set("l6nl/5+P1gk/2np1S3/p1p4Pp/3P2Sp1/1PPb2P1P/P5GS1/R8/LN4bKL w GR5pnsg 1");

    // "position sfen"の略。
    else if (token == "sfen") position_cmd(pos,is);

    // ログファイルの書き出しのon
    else if (token == "log") start_logger(true);

    // 現在の局面について評価関数を呼び出して、その値を返す。
    else if (token == "eval") cout << "eval = " << Eval::evaluate(pos) << endl;
    else if (token == "evalstat") Eval::print_eval_stat(pos);

    // この局面での指し手をすべて出力
    else if (token == "moves") {
      for (auto m : MoveList<LEGAL_ALL>(pos))
        cout << m.move << ' ';
      cout << endl;
    }

    // この局面が詰んでいるかの判定
    else if (token == "mated") cout << pos.is_mated() << endl;

    // この局面のhash keyの値を出力
    else if (token == "key") cout << hex << pos.state()->key() << dec << endl;

#if defined(MATE_1PLY) && defined(LONG_EFFECT_LIBRARY)
    // この局面での1手詰め判定
    else if (token == "mate1") cout << pos.mate1ply() << endl;
#endif

#ifdef ENABLE_TEST_CMD
    // 指し手生成のテスト
    else if (token == "s") generate_moves_cmd(pos);

    // パフォーマンステスト(Stockfishにある、合法手N手で到達できる局面を求めるやつ)
    else if (token == "perft") perft(pos, is);

    // テストコマンド
    else if (token == "test") test_cmd(pos, is);

    // ベンチコマンド
    else if (token == "bench") bench_cmd(pos, is);
#endif

#ifdef ENABLE_MAKEBOOK_CMD
    // 定跡を作るコマンド
    else if (token == "makebook") Book::makebook_cmd(pos, is);
#endif
    ;

  } while (token != "quit" );
  
  // quitが来た時点ではまだ探索中かも知れないのでmain threadの停止を待つ。
  Threads.main()->wait_for_search_finished();
}

// --------------------
// USI関係の記法変換部
// --------------------

// USIの指し手文字列などに使われている盤上の升を表す文字列をSquare型に変換する
// 変換できなかった場合はSQ_NBが返る。
Square usi_to_sq(char f, char r)
{
  File file = toFile(f);
  Rank rank = toRank(r);

  if (is_ok(file) && is_ok(rank))
    return file | rank;

  return SQ_NB;
}

// usi形式から指し手への変換。本来この関数は要らないのだが、
// 棋譜を大量に読み込む都合、この部分をそこそこ高速化しておきたい。
Move move_from_usi(const string& str)
{
  // さすがに3文字以下の指し手はおかしいだろ。
  if (str.length() <= 3)
    return MOVE_NONE;

  Square to = usi_to_sq(str[2], str[3]);
  if (!is_ok(to))
    return MOVE_NONE;

  bool promote = str.length() == 5 && str[4] == '+';
  bool drop = str[1] == '*';

  Move move = MOVE_NONE;
  if (!drop)
  {
    Square from = usi_to_sq(str[0],str[1]);
    if (is_ok(from))
      move = promote ? make_move_promote(from, to) : make_move(from, to);
  }
  else
  {
    for (int i = 1; i <= 7; ++i)
      if (PieceToCharBW[i] == str[0])
      {
        move = make_move_drop((Piece)i, to);
        break;
      }
  }

  return move;
}


// 局面posとUSIプロトコルによる指し手を与えて
// もし可能なら等価で合法な指し手を返す。(合法でないときはMOVE_NONEを返す)
Move move_from_usi(const Position& pos, const std::string& str)
{
  // 全合法手のなかからusi文字列に変換したときにstrと一致する指し手を探してそれを返す
  //for (const ExtMove& ms : MoveList<LEGAL_ALL>(pos))
  //  if (str == move_to_usi(ms.move))
  //    return ms.move;

  // ↑のコードは大変美しいコードではあるが、棋譜を大量に読み込むときに時間がかかるうるのでもっと高速な実装をする。

  if (str == "resign")
    return MOVE_RESIGN;

  // usi文字列をmoveに変換するやつがいるがな..
  Move move = move_from_usi(str);

  // pseudo_legal(),legal()チェックのためにはCheckInfoのupdateが必要。
  const_cast<Position*>(&pos)->check_info_update();
  if (pos.pseudo_legal(move) && pos.legal(move))
    return move;

  // いかなる状況であろうとこのような指し手はエラー表示をして弾いていいと思うが…。
  // cout << "\nIlligal Move : " << str << "\n";

  return MOVE_NONE;
}
